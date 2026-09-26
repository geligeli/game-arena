// The problem server: one process per problem, and a pure coordinator.
/*
bazel run //game_arena/server:problem_server -- \
    --problem_config=game_arena/problems/nim.textproto \
    --data_dir=tournament_data
*/
//
// It accepts submissions, stores them, schedules their evaluation onto the
// sandbox fleet, and publishes the standings. It does not build anything, run
// anything, or referee anything -- all of that happens on a worker, in a
// container, behind the SandboxFleet stream that workers dial in on.
//
// That is not a stylistic claim, it is a build-time one: this binary links no
// game and no problem code, and
// //game_arena/server:no_problem_code_test fails the build if
// it ever does. The rules of any particular problem live in
// //game_arena/referee, which only workers depend on.
//
// What the problem is comes from --problem_config (see proto/problem.proto).
// Everything that used to be a flag here about *how a game is played* now lives
// in that file, because it is the same question for a graded problem and a
// tournament problem only if you never hardcode one of them.

#include <grpcpp/grpcpp.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <system_error>

#include "absl/flags/flag.h"
#include "absl/flags/parse.h"
#include "absl/log/globals.h"
#include "absl/log/initialize.h"
#include "absl/log/log.h"
#include "game_arena/server/arena_service.h"
#include "game_arena/server/candidate_store.h"
#include "game_arena/server/client_registry.h"
#include "game_arena/server/dashboard.h"
#include "game_arena/server/fleet_service.h"
#include "game_arena/server/job_log.h"
#include "game_arena/server/problem_config.h"
#include "game_arena/server/scheduler.h"
#include "game_arena/standings/elo_standings.h"
#include "game_arena/standings/elo_store.h"
#include "game_arena/standings/game_history.h"
#include "game_arena/standings/http_leaderboard.h"
#include "game_arena/standings/metric_standings.h"

ABSL_FLAG(std::string, problem_config, "",
          "Path to the problem's .textproto (required). See "
          "game_arena/problems/ for the shipped ones");
ABSL_FLAG(int, grpc_port, 50051,
          "Port for the Arena and SandboxFleet services");
ABSL_FLAG(int, http_port, 8090, "Port for the HTTP leaderboard");
ABSL_FLAG(std::string, data_dir, "tournament_data",
          "Directory for submissions, ratings.pb and games/");
ABSL_FLAG(std::string, clients, "",
          "Path to the client registry (.textproto). Writes require an "
          "x-arena-token header naming a client in it; reads never do. Empty "
          "leaves Submit open to anyone who can reach the port, "
          "which is fine for a single-agent loop and nothing else. Mint "
          "clients with //game_arena/tools:arena_admin");
ABSL_FLAG(double, k_factor, 32.0, "ELO K factor");
ABSL_FLAG(int, keepalive_s, 60,
          "Interval between HTTP/2 keepalive pings on idle connections. "
          "Reclaims connections whose peer vanished without a TCP FIN, which "
          "otherwise linger indefinitely");
ABSL_FLAG(int, shutdown_grace_s, 5,
          "How long a shutdown waits for in-flight RPCs to finish before "
          "cancelling them");

namespace {

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

// Blocks until SIGINT/SIGTERM.
void WaitForShutdownSignal() {
  std::unique_lock lock(g_shutdown_mutex);
  g_shutdown_cv.wait(lock, [] { return g_shutdown_requested; });
}

// Turns the problem's evaluation spec into the scheduler's knobs. The scheduler
// stays problem-agnostic: it knows about orders and timeouts, not about games
// or benchmarks.
tournament_arena::SchedulerConfig SchedulerConfigFor(
    const tournament_arena::proto::ProblemConfig &problem) {
  tournament_arena::SchedulerConfig config;
  config.set_build_timeout_s(static_cast<int>(problem.build().timeout_s()));
  *config.mutable_build_targets() = problem.build().targets();
  *config.mutable_bazel_flags() = problem.build().bazel_flags();

  // The sandbox, translated rather than embedded: see SandboxOrder.
  const auto &sandbox = problem.sandbox();
  auto *order_sandbox = config.mutable_sandbox();
  order_sandbox->set_image(sandbox.image());
  order_sandbox->set_memory_limit_mb(sandbox.memory_limit_mb());
  order_sandbox->set_cpus(sandbox.cpus());
  order_sandbox->set_pids_limit(sandbox.pids_limit());
  order_sandbox->set_run_as_user(sandbox.run_as_user());
  order_sandbox->set_allow_build_network(sandbox.allow_build_network());
  if (problem.has_match()) {
    const auto &match = problem.match();
    *config.mutable_placement_opponents() = match.placement_opponents();
    config.set_placement_games(static_cast<int>(match.games_per_order()));
    config.set_run_timeout_s(static_cast<int>(match.timeout_s()));
    config.set_referee_target(match.referee_target());
    config.set_turn_timeout_ms(match.turn_timeout_ms());
    config.set_game_time_budget_ms(match.game_time_budget_ms());
    config.set_max_moves_per_game(match.max_moves_per_game());
    *config.mutable_registry_options() = match.registry_options();
    // Kept under the worker's own run timeout, so a stuck match comes back as a
    // partial tally rather than an order-level failure.
    config.set_match_deadline_s(std::max(1, config.run_timeout_s() - 30));
    // The bot is the build target named per submission; a problem that builds
    // nothing per submission plays with the first target it builds.
    for (const std::string &target : config.build_targets()) {
      if (target.find("{submission_id}") != std::string::npos) {
        config.set_bot_target(target);
        break;
      }
    }
    if (!config.has_bot_target() && config.build_targets_size() > 0) {
      config.set_bot_target(config.build_targets(0));
    }
  } else {
    // A graded problem has no opponents: one order is the whole evaluation.
    const auto &grade = problem.grade();
    config.set_placement_games(static_cast<int>(grade.repeats()));
    config.set_run_timeout_s(static_cast<int>(grade.timeout_s()));

    auto *order = config.mutable_grade();
    for (const std::string &arg : grade.argv()) {
      order->add_argv(arg);  // "{submission_id}" expanded per submission
    }
    order->set_repeats(static_cast<int>(grade.repeats()));
    order->set_aggregate(
        static_cast<tournament_arena::proto::GradeOrder::Aggregate>(
            static_cast<int>(grade.aggregate())));
    for (const auto &metric : grade.metrics()) {
      order->add_metric_names(metric.name());
    }
    order->set_timeout_s(static_cast<int>(grade.timeout_s()));
    order->set_require_machine_class(grade.require_machine_class());
  }
  return config;
}

}  // namespace

int main(int argc, char **argv) {
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
  // Every game the fleet played, as each order's referee recorded it; served
  // by the leaderboard's /api/games.
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

  // Who may submit, and how much. Reread on an unknown token so adding a client
  // does not mean a restart that drops every attached worker mid-order.
  std::unique_ptr<tournament_arena::ClientRegistry> clients;
  if (!absl::GetFlag(FLAGS_clients).empty()) {
    clients = std::make_unique<tournament_arena::ClientRegistry>(
        absl::GetFlag(FLAGS_clients), problem->clients().default_quota());
    if (!clients->Load(&error)) {
      LOG(ERROR) << error;
      return 2;
    }
  } else {
    LOG(WARNING) << "No --clients registry: Submit is open to "
                    "anyone who can reach this port, and `author` is whatever "
                    "the caller says it is";
  }

  // Every job, with its submission and each build's output, for the
  // dashboard: the candidate store keeps only a participant's latest code.
  tournament_arena::JobLog job_log(data_dir / "jobs");
  tournament_arena::Scheduler scheduler(SchedulerConfigFor(*problem),
                                        &candidates, standings.get(), &history,
                                        &job_log);
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
  switch (problem->source().visibility()) {
    case tournament_arena::proto::SourcePolicy::OWN:
      info.set_source_visibility(
          tournament_arena::proto::ProblemInfo::SOURCE_OWN);
      break;
    case tournament_arena::proto::SourcePolicy::NONE:
      info.set_source_visibility(
          tournament_arena::proto::ProblemInfo::SOURCE_NONE);
      break;
    default:
      info.set_source_visibility(
          tournament_arena::proto::ProblemInfo::SOURCE_ALL);
      break;
  }
  if (graded) {
    const auto *primary = tournament_arena::PrimaryMetric(*problem);
    info.set_lower_is_better(primary->direction() ==
                             tournament_arena::proto::MetricSpec::MINIMIZE);
  }

  tournament_arena::ArenaService arena(
      &candidates, &scheduler, standings.get(), graded,
      problem->has_match() ? problem->match().game() : "", std::move(info),
      clients.get());
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

  // Jobs, participants and game replays beside the leaderboard. As
  // unauthenticated as it, so source shows only where everyone may read it.
  const tournament_arena::Dashboard dashboard(
      &candidates, &job_log, &history, standings.get(),
      problem->source().visibility() ==
          tournament_arena::proto::SourcePolicy::ALL);
  tournament_broker::HttpLeaderboard leaderboard(
      absl::GetFlag(FLAGS_http_port), &history, &candidates, standings.get(),
      problem->display_name().empty() ? problem->problem_id()
                                      : problem->display_name(),
      [&dashboard](std::string_view target) {
        return dashboard.Route(target);
      });
  if (!leaderboard.Start()) {
    return 1;
  }

  std::signal(SIGINT, OnShutdownSignal);
  std::signal(SIGTERM, OnShutdownSignal);

  LOG(INFO) << "Problem '" << problem->problem_id() << "' ("
            << (problem->has_match() ? "match" : "grade")
            << ") on :" << absl::GetFlag(FLAGS_grpc_port) << ", leaderboard on "
            << "http://localhost:" << absl::GetFlag(FLAGS_http_port)
            << ", data dir " << data_dir << ", sandbox image "
            << problem->sandbox().image() << ", " << candidates.size()
            << " submission(s) loaded";
  if (problem->source().visibility() !=
      tournament_arena::proto::SourcePolicy::ALL) {
    LOG(INFO) << "Candidate source is "
              << (problem->source().visibility() ==
                          tournament_arena::proto::SourcePolicy::OWN
                      ? "served only to its own author, on a token"
                      : "served to nobody")
              << ": the default is that every participant reads every "
                 "submission";
  }
  WaitForShutdownSignal();
  LOG(INFO) << "Shutting down";

  // The grace period is a backstop for stragglers -- an arena RPC mid-flight, a
  // worker's Attach stream. Cancelled calls surface in their handlers, which
  // always finish the RPC. The no-argument Shutdown() would wait forever.
  server->Shutdown(std::chrono::system_clock::now() +
                   std::chrono::seconds(absl::GetFlag(FLAGS_shutdown_grace_s)));
  leaderboard.Stop();
  return 0;
}
