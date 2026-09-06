// The problem server: one process per problem, and a pure coordinator.
/*
bazel run //game_mcts/tournament_server/server:problem_server -- \
    --problem_config=game_mcts/tournament_server/problems/nim.textproto \
    --data_dir=tournament_data --base_commit=$(git rev-parse HEAD)
*/
//
// It accepts submissions, stores them, schedules their evaluation onto the
// sandbox fleet, and publishes the standings. It does not build anything, run
// anything, or referee anything -- all of that happens on a worker, in a
// container, behind the SandboxFleet stream that workers dial in on.
//
// That is not a stylistic claim, it is a build-time one: this binary links no
// game and no problem code, and
// //game_mcts/tournament_server/server:no_problem_code_test fails the build if
// it ever does. The rules of any particular problem live in
// //game_mcts/tournament_server/referee, which only workers depend on.
//
// What the problem is comes from --problem_config (see proto/problem.proto).
// Everything that used to be a flag here about *how a game is played* now lives
// in that file, because it is the same question for a graded problem and a
// tournament problem only if you never hardcode one of them.

#include <grpcpp/grpcpp.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <system_error>

#include "absl/flags/flag.h"
#include "absl/flags/parse.h"
#include "absl/log/globals.h"
#include "absl/log/initialize.h"
#include "absl/log/log.h"
#include "game_mcts/tournament_server/server/arena_service.h"
#include "game_mcts/tournament_server/server/candidate_store.h"
#include "game_mcts/tournament_server/server/client_registry.h"
#include "game_mcts/tournament_server/server/elo_standings.h"
#include "game_mcts/tournament_server/server/elo_store.h"
#include "game_mcts/tournament_server/server/fleet_service.h"
#include "game_mcts/tournament_server/server/game_history.h"
#include "game_mcts/tournament_server/server/http_leaderboard.h"
#include "game_mcts/tournament_server/server/metric_standings.h"
#include "game_mcts/tournament_server/server/problem_config.h"
#include "game_mcts/tournament_server/server/scheduler.h"

ABSL_FLAG(std::string, problem_config, "",
          "Path to the problem's .textproto (required). See "
          "game_mcts/tournament_server/problems/ for the shipped ones");
ABSL_FLAG(int, grpc_port, 50051,
          "Port for the Arena and SandboxFleet services");
ABSL_FLAG(int, http_port, 8080, "Port for the HTTP leaderboard");
ABSL_FLAG(std::string, data_dir, "tournament_data",
          "Directory for submissions, ratings.pb and games/");
ABSL_FLAG(std::string, base_commit, "",
          "Overrides the config's repo.base_commit. Pass an explicit sha -- "
          "e.g. --base_commit=$(git rev-parse HEAD) -- so every submission is "
          "built against one known tree; ratings from different trees are not "
          "comparable. Empty: the config's value");
ABSL_FLAG(std::string, clients, "",
          "Path to the client registry (.textproto). Writes require an "
          "x-arena-token header naming a client in it; reads never do. Empty "
          "leaves Submit and Evaluate open to anyone who can reach the port, "
          "which is fine for a single-agent loop and nothing else. Mint "
          "clients with //game_mcts/tournament_server/tools:arena_admin");
ABSL_FLAG(double, k_factor, 32.0, "ELO K factor");
ABSL_FLAG(int, keepalive_s, 60,
          "Interval between HTTP/2 keepalive pings on idle connections. "
          "Reclaims connections whose peer vanished without a TCP FIN, which "
          "otherwise linger indefinitely");
ABSL_FLAG(int, shutdown_grace_s, 5,
          "How long a shutdown waits for in-flight RPCs to finish before "
          "cancelling them");

namespace {

// Set by SIGHUP, drained by the main thread. Signal context can do almost
// nothing safely, so it sets a flag and the reload happens on a real thread.
std::atomic<bool> g_reload_requested{false};

extern "C" void OnReloadSignal(int /*signum*/) {
  g_reload_requested.store(true);
}

std::mutex g_shutdown_mutex;
std::condition_variable g_shutdown_cv;
bool g_shutdown_requested = false;

// Runs in signal context, so it does the least it can: set a flag and wake the
// main thread, which does the actual shutdown.
extern "C" void OnShutdownSignal(int /*signum*/) {
  {
    std::lock_guard lock(g_shutdown_mutex);
    g_shutdown_requested = true;
  }
  g_shutdown_cv.notify_all();
}

// Blocks until SIGINT/SIGTERM, reloading the client registry whenever SIGHUP
// arrives. The wait is timed rather than indefinite so a SIGHUP that lands
// between the flag check and the wait is still picked up.
void WaitForShutdownSignal(tournament_arena::ClientRegistry *clients) {
  std::unique_lock lock(g_shutdown_mutex);
  while (!g_shutdown_requested) {
    g_shutdown_cv.wait_for(lock, std::chrono::seconds(1),
                           [] { return g_shutdown_requested; });
    if (!g_reload_requested.exchange(false)) {
      continue;
    }
    if (clients == nullptr) {
      LOG(WARNING) << "SIGHUP ignored: no --clients registry to reload";
      continue;
    }
    std::string error;
    if (!clients->Load(&error)) {
      // The registry keeps whatever it had, so a typo does not lock everyone
      // out; it just does not take effect.
      LOG(ERROR) << "Reload failed, keeping the current clients: " << error;
    }
  }
}

// Turns the problem's evaluation spec into the scheduler's knobs. The scheduler
// stays problem-agnostic: it knows about orders and timeouts, not about games
// or benchmarks.
auto SchedulerConfigFor(const tournament_arena::proto::ProblemConfig &problem)
    -> tournament_arena::SchedulerConfig {
  tournament_arena::SchedulerConfig config;
  config.build_timeout_s = static_cast<int>(problem.build().timeout_s());
  config.build_targets.assign(problem.build().targets().begin(),
                              problem.build().targets().end());
  config.require_container = problem.sandbox().require_container();
  if (problem.has_match()) {
    const auto &match = problem.match();
    config.placement_opponents.assign(match.placement_opponents().begin(),
                                      match.placement_opponents().end());
    config.placement_games = static_cast<int>(match.games_per_order());
    config.default_games = static_cast<int>(match.games_per_order());
    config.run_timeout_s = static_cast<int>(match.timeout_s());
    config.referee_target = match.referee_target();
    // Kept under the worker's own run timeout, so a stuck match comes back as a
    // partial tally rather than an order-level failure.
    config.match_deadline_s = std::max(1, config.run_timeout_s - 30);
    // A match problem plays with the first target it builds.
    if (!config.build_targets.empty()) {
      config.bot_target = config.build_targets.front();
    }
  } else {
    // A graded problem has no opponents: one order is the whole evaluation.
    const auto &grade = problem.grade();
    config.placement_opponents.clear();
    config.placement_games = static_cast<int>(grade.repeats());
    config.default_games = static_cast<int>(grade.repeats());
    config.run_timeout_s = static_cast<int>(grade.timeout_s());

    tournament_arena::proto::GradeOrder order;
    for (const std::string &arg : grade.argv()) {
      order.add_argv(arg);  // "{submission_id}" expanded per submission
    }
    order.set_repeats(static_cast<int>(grade.repeats()));
    order.set_aggregate(
        static_cast<tournament_arena::proto::GradeOrder::Aggregate>(
            static_cast<int>(grade.aggregate())));
    for (const auto &metric : grade.metrics()) {
      order.add_metric_names(metric.name());
    }
    order.set_timeout_s(static_cast<int>(grade.timeout_s()));
    order.set_require_machine_class(grade.require_machine_class());
    config.grade = std::move(order);
  }
  return config;
}

}  // namespace

auto main(int argc, char **argv) -> int {
  absl::ParseCommandLine(argc, argv);
  absl::InitializeLog();
  absl::SetStderrThreshold(absl::LogSeverityAtLeast::kInfo);

  const std::string config_path = absl::GetFlag(FLAGS_problem_config);
  if (config_path.empty()) {
    LOG(ERROR) << "--problem_config is required";
    return 2;
  }
  std::string error;
  auto problem = tournament_arena::LoadProblemConfig(config_path, &error);
  if (!problem) {
    LOG(ERROR) << error;
    return 2;
  }
  if (!absl::GetFlag(FLAGS_base_commit).empty()) {
    problem->mutable_repo()->set_base_commit(absl::GetFlag(FLAGS_base_commit));
  }
  if (problem->repo().base_commit() == "HEAD") {
    // Deliberately a warning rather than an error: it is the right setting for
    // a dev loop against a moving checkout, and wrong for anything whose
    // numbers are compared. Workers resolve it independently, so two of them
    // can build two different trees and the ratings will not say so.
    LOG(WARNING) << "repo.base_commit is \"HEAD\": submissions may be built "
                    "against different trees and their ratings are not "
                    "comparable. Pass --base_commit=<sha> for a real run";
  }

  const std::filesystem::path data_dir = absl::GetFlag(FLAGS_data_dir);
  std::error_code ec;
  std::filesystem::create_directories(data_dir, ec);
  if (ec) {
    LOG(ERROR) << "Cannot create --data_dir " << data_dir << ": "
               << ec.message();
    return 1;
  }

  tournament_broker::EloStore elo_store(data_dir / "ratings.pb",
                                        absl::GetFlag(FLAGS_k_factor));
  elo_store.Load();
  // Kept for the leaderboard's /api/games. Matches are refereed in the sandbox
  // now and only their tallies come back, so nothing here writes to it until a
  // worker ships records; see ARENA.md.
  tournament_broker::GameHistory history(data_dir / "games");

  tournament_arena::SubmissionRules rules;
  rules.policy = problem->submission();
  rules.files_submit_dir = problem->submission().files_submit_dir();
  rules.harness = problem->submission().harness();
  tournament_arena::CandidateStore candidates(
      data_dir / "candidates", tournament_arena::CandidateLimits{}, rules);
  candidates.Load();

  // How this problem is scored. Everything above it -- the scheduler, the
  // Arena service, the HTTP table -- sees rows, not ratings or milliseconds.
  std::unique_ptr<tournament_arena::Standings> standings;
  std::unique_ptr<tournament_arena::MetricStandings> metric_standings;
  const bool graded = problem->has_grade();
  if (graded) {
    const tournament_arena::proto::MetricSpec *primary =
        tournament_arena::PrimaryMetric(*problem);
    metric_standings = std::make_unique<tournament_arena::MetricStandings>(
        data_dir / "metrics.pb", primary->name(),
        primary->direction() == tournament_arena::proto::MetricSpec::MINIMIZE);
    metric_standings->Load();
    standings = std::move(metric_standings);
  } else {
    standings = std::make_unique<tournament_arena::EloStandings>(
        &elo_store, &candidates, problem->problem_id());
  }

  // Who may submit, and how much. Reloadable on SIGHUP so adding a client does
  // not mean a restart that drops every attached worker mid-order.
  std::unique_ptr<tournament_arena::ClientRegistry> clients;
  if (!absl::GetFlag(FLAGS_clients).empty()) {
    clients = std::make_unique<tournament_arena::ClientRegistry>(
        absl::GetFlag(FLAGS_clients), problem->clients().default_quota());
    if (!clients->Load(&error)) {
      LOG(ERROR) << error;
      return 2;
    }
  } else {
    LOG(WARNING) << "No --clients registry: Submit and Evaluate are open to "
                    "anyone who can reach this port, and `author` is whatever "
                    "the caller says it is";
  }

  tournament_arena::Scheduler scheduler(
      SchedulerConfigFor(*problem), &candidates, &elo_store, standings.get());
  // What an agent needs to know about the problem, curated from the config:
  // the operator's image names and timeouts are not a submitter's business.
  tournament_arena::proto::ProblemInfo info;
  info.set_problem_id(problem->problem_id());
  info.set_display_name(problem->display_name());
  info.set_description(problem->description());
  info.set_max_patch_bytes(problem->submission().max_patch_bytes());
  info.set_max_files(problem->submission().max_files());
  info.set_max_hunks(problem->submission().max_hunks());
  for (const std::string &pattern : problem->submission().allow_paths()) {
    info.add_allow_paths(pattern);
  }
  for (const std::string &pattern : problem->submission().deny_paths()) {
    info.add_deny_paths(pattern);
  }
  info.set_files_submit_dir(problem->submission().files_submit_dir());
  if (graded) {
    const auto *primary = tournament_arena::PrimaryMetric(*problem);
    info.set_lower_is_better(primary->direction() ==
                             tournament_arena::proto::MetricSpec::MINIMIZE);
  }

  tournament_arena::ArenaService arena(&candidates, &scheduler, standings.get(),
                                       problem->repo().base_commit(), graded,
                                       std::move(info), clients.get());
  tournament_arena::FleetService fleet(&scheduler);

  grpc::ServerBuilder builder;
  builder.AddListeningPort(
      "0.0.0.0:" + std::to_string(absl::GetFlag(FLAGS_grpc_port)),
      grpc::InsecureServerCredentials());
  // Reclaim connections whose peer disappeared without closing the socket. A
  // worker host that is powered off mid-order leaves no FIN behind, so without
  // keepalive its Attach stream stays open until the OS gives up on the TCP
  // connection, which can be hours.
  const int keepalive_ms = absl::GetFlag(FLAGS_keepalive_s) * 1000;
  builder.AddChannelArgument(GRPC_ARG_KEEPALIVE_TIME_MS, keepalive_ms);
  builder.AddChannelArgument(GRPC_ARG_KEEPALIVE_TIMEOUT_MS, 20000);
  builder.AddChannelArgument(GRPC_ARG_KEEPALIVE_PERMIT_WITHOUT_CALLS, 1);
  // A worker with no free slots is idle, not dead.
  builder.AddChannelArgument(
      GRPC_ARG_HTTP2_MIN_RECV_PING_INTERVAL_WITHOUT_DATA_MS, keepalive_ms / 2);
  builder.RegisterService(&arena);
  builder.RegisterService(&fleet);
  std::unique_ptr<grpc::Server> server = builder.BuildAndStart();
  if (!server) {
    LOG(ERROR) << "Cannot bind gRPC port " << absl::GetFlag(FLAGS_grpc_port);
    return 1;
  }

  tournament_broker::HttpLeaderboard leaderboard(
      absl::GetFlag(FLAGS_http_port), &history, &candidates, standings.get(),
      problem->display_name().empty() ? problem->problem_id()
                                      : problem->display_name());
  if (!leaderboard.Start()) {
    return 1;
  }

  std::signal(SIGINT, OnShutdownSignal);
  std::signal(SIGTERM, OnShutdownSignal);
  std::signal(SIGHUP, OnReloadSignal);

  LOG(INFO) << "Problem '" << problem->problem_id() << "' ("
            << (problem->has_match() ? "match" : "grade")
            << ") on :" << absl::GetFlag(FLAGS_grpc_port) << ", leaderboard on "
            << "http://localhost:" << absl::GetFlag(FLAGS_http_port)
            << ", data dir " << data_dir << ", base commit "
            << problem->repo().base_commit() << ", " << candidates.size()
            << " submission(s) loaded";
  WaitForShutdownSignal(clients.get());
  LOG(INFO) << "Shutting down";

  // The grace period is a backstop for stragglers -- an arena RPC mid-flight, a
  // worker's Attach stream. Cancelled calls surface in their handlers, which
  // always finish the RPC. The no-argument Shutdown() would wait forever.
  server->Shutdown(std::chrono::system_clock::now() +
                   std::chrono::seconds(absl::GetFlag(FLAGS_shutdown_grace_s)));
  leaderboard.Stop();
  return 0;
}
