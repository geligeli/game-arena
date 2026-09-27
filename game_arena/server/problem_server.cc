// The coordinator. It links no problem code (:no_problem_code_test).

#include <grpcpp/grpcpp.h>
#include <signal.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <memory>
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

tournament_arena::SchedulerConfig SchedulerConfigFor(
    const tournament_arena::proto::ProblemConfig& problem) {
  tournament_arena::SchedulerConfig config;
  *config.mutable_build_targets() = problem.build().targets();
  tournament_arena::proto::WorkOrder* order = config.mutable_order();
  order->set_build_timeout_s(static_cast<int>(problem.build().timeout_s()));
  *order->mutable_bazel_flags() = problem.build().bazel_flags();

  // The sandbox, translated rather than embedded: see SandboxOrder.
  const auto& sandbox = problem.sandbox();
  auto* order_sandbox = order->mutable_sandbox();
  order_sandbox->set_image(sandbox.image());
  order_sandbox->set_memory_limit_mb(sandbox.memory_limit_mb());
  order_sandbox->set_cpus(sandbox.cpus());
  order_sandbox->set_pids_limit(sandbox.pids_limit());
  order_sandbox->set_run_as_user(sandbox.run_as_user());
  order_sandbox->set_allow_build_network(sandbox.allow_build_network());
  if (problem.has_match()) {
    const auto& match = problem.match();
    *config.mutable_placement_opponents() = match.placement_opponents();
    config.set_placement_games(static_cast<int>(match.games_per_order()));
    order->set_run_timeout_s(static_cast<int>(match.timeout_s()));
    order->set_referee_target(match.referee_target());
    order->set_turn_timeout_ms(match.turn_timeout_ms());
    order->set_game_time_budget_ms(match.game_time_budget_ms());
    order->set_max_moves_per_game(match.max_moves_per_game());
    order->set_max_view_bytes(match.max_view_bytes());
    *order->mutable_registry_options() = match.registry_options();
    // Under the run timeout, so a stuck match comes back as a partial tally.
    order->set_match_deadline_s(std::max(1, order->run_timeout_s() - 30));
    const auto& targets = config.build_targets();
    const auto bot = std::find_if(
        targets.begin(), targets.end(), [](const std::string& target) {
          return target.find("{submission_id}") != std::string::npos;
        });
    if (!targets.empty()) {
      config.set_bot_target(bot != targets.end() ? *bot : targets[0]);
    }
  } else {
    const auto& grade = problem.grade();
    config.set_placement_games(static_cast<int>(grade.repeats()));
    order->set_run_timeout_s(static_cast<int>(grade.timeout_s()));

    auto* graded = order->mutable_grade();
    *graded->mutable_argv() = grade.argv();  // "{submission_id}" still in it
    graded->set_repeats(static_cast<int>(grade.repeats()));
    graded->set_aggregate(
        static_cast<tournament_arena::proto::GradeOrder::Aggregate>(
            static_cast<int>(grade.aggregate())));
    for (const auto& metric : grade.metrics()) {
      graded->add_metric_names(metric.name());
    }
    graded->set_timeout_s(static_cast<int>(grade.timeout_s()));
    graded->set_require_machine_class(grade.require_machine_class());
  }
  return config;
}

}  // namespace

int main(int argc, char** argv) {
  // Before any thread starts, so only the sigwait below sees them.
  sigset_t shutdown_signals;
  sigemptyset(&shutdown_signals);
  sigaddset(&shutdown_signals, SIGINT);
  sigaddset(&shutdown_signals, SIGTERM);
  pthread_sigmask(SIG_BLOCK, &shutdown_signals, nullptr);

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
  tournament_broker::GameHistory history(data_dir / "games");

  tournament_arena::CandidateStore candidates(
      data_dir / "candidates", tournament_arena::CandidateLimits{},
      problem->submission());
  candidates.Load();

  std::unique_ptr<tournament_arena::Standings> standings;
  std::unique_ptr<tournament_arena::MetricStandings> metric_standings;
  const bool graded = problem->has_grade();
  if (graded) {
    const tournament_arena::proto::MetricSpec* primary =
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

  tournament_arena::JobLog job_log(data_dir / "jobs");
  tournament_arena::Scheduler scheduler(SchedulerConfigFor(*problem),
                                        &candidates, standings.get(), &history,
                                        &job_log);
  // Curated: the operator's image names and timeouts are not a submitter's.
  tournament_arena::proto::ProblemInfo info;
  info.set_problem_id(problem->problem_id());
  info.set_display_name(problem->display_name());
  info.set_description(problem->description());
  info.set_max_patch_bytes(problem->submission().max_patch_bytes());
  info.set_max_files(problem->submission().max_files());
  info.set_max_hunks(problem->submission().max_hunks());
  *info.mutable_allow_paths() = problem->submission().allow_paths();
  *info.mutable_deny_paths() = problem->submission().deny_paths();
  info.set_files_submit_dir(problem->submission().files_submit_dir());
  const auto visibility = problem->source().visibility();
  info.set_source_visibility(
      visibility == tournament_arena::proto::SourcePolicy::OWN
          ? tournament_arena::proto::ProblemInfo::SOURCE_OWN
      : visibility == tournament_arena::proto::SourcePolicy::NONE
          ? tournament_arena::proto::ProblemInfo::SOURCE_NONE
          : tournament_arena::proto::ProblemInfo::SOURCE_ALL);
  info.set_graded(graded);
  if (graded) {
    const auto* primary = tournament_arena::PrimaryMetric(*problem);
    info.set_lower_is_better(primary->direction() ==
                             tournament_arena::proto::MetricSpec::MINIMIZE);
  }

  tournament_arena::ArenaService arena(
      &candidates, &scheduler, standings.get(),
      problem->has_match() ? problem->match().game() : "", std::move(info),
      clients.get());
  tournament_arena::FleetService fleet(&scheduler);

  grpc::ServerBuilder builder;
  builder.AddListeningPort(
      "0.0.0.0:" + std::to_string(absl::GetFlag(FLAGS_grpc_port)),
      grpc::InsecureServerCredentials());
  // A host powered off mid-order sends no FIN; keepalive reclaims its stream.
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
  int signum = 0;
  sigwait(&shutdown_signals, &signum);
  LOG(INFO) << "Shutting down";

  // With no deadline, Shutdown() would wait forever on a worker's Attach.
  server->Shutdown(std::chrono::system_clock::now() +
                   std::chrono::seconds(absl::GetFlag(FLAGS_shutdown_grace_s)));
  leaderboard.Stop();
  return 0;
}
