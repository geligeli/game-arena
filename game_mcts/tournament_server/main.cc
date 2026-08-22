// Tournament server: the broker, the candidate registry, and the sandbox
// fleet's control plane, in one process.
//
// Three gRPC services share a port because they share state -- the arena's
// standings *are* the broker's ELO store, since a candidate's id is its player
// name. They are separate services because they have separate audiences:
//
//   TournamentBroker  bots playing games
//   Arena             agents submitting and reading strategies (via MCP)
//   SandboxFleet      build-and-run workers, which dial in from any host
//
//   bazel run //game_mcts/tournament_server:tournament_server
//       -- --grpc_port 50051 --http_port 8080 --data_dir tournament_data

#include <chrono>
#include <condition_variable>
#include <csignal>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>

#include <grpcpp/grpcpp.h>

#include "absl/flags/flag.h"
#include "absl/flags/parse.h"
#include "absl/log/globals.h"
#include "absl/log/initialize.h"
#include "absl/log/log.h"
#include "cpp/tournament_server/arena_service.h"
#include "cpp/tournament_server/broker_service.h"
#include "cpp/tournament_server/candidate_store.h"
#include "cpp/tournament_server/elo_store.h"
#include "cpp/tournament_server/fleet_service.h"
#include "cpp/tournament_server/game_history.h"
#include "cpp/tournament_server/game_registry.h"
#include "cpp/tournament_server/http_leaderboard.h"
#include "cpp/tournament_server/matchmaker.h"
#include "cpp/tournament_server/scheduler.h"

ABSL_FLAG(int, grpc_port, 50051, "Port for the gRPC tournament broker");
ABSL_FLAG(int, http_port, 8080, "Port for the HTTP leaderboard");
ABSL_FLAG(std::string, data_dir, "tournament_data",
          "Directory for ratings.pb and games/ history");
ABSL_FLAG(int, turn_timeout_ms, 10000,
          "Per-turn wall-clock time limit; exceeding it loses the game");
ABSL_FLAG(int, game_time_budget_ms, 0,
          "Total thinking time per seat per game; exceeding it loses the "
          "game. 0 disables the budget, leaving only --turn_timeout_ms");
ABSL_FLAG(int, rendezvous_timeout_ms, 60000,
          "How long an opponent=player:<name> client waits for its named "
          "partner before its stream is closed");
ABSL_FLAG(int, max_moves_per_game, 50000,
          "Safety cap on moves per game before declaring a draw");
ABSL_FLAG(int, worker_threads, 0,
          "Threads serving all games; bounds how much CPU-heavy built-in work "
          "runs at once. 0 uses hardware_concurrency()");
ABSL_FLAG(int, hello_timeout_ms, 30000,
          "How long a connection may stay open without sending its hello "
          "before the stream is closed. Bounds connections that open a stream "
          "and then say nothing");
ABSL_FLAG(int, keepalive_s, 60,
          "Interval between HTTP/2 keepalive pings on idle connections. "
          "Reclaims connections whose peer vanished without a TCP FIN, which "
          "otherwise linger indefinitely");
ABSL_FLAG(int, shutdown_grace_s, 5,
          "How long a shutdown waits for in-flight RPCs to finish before "
          "cancelling them");
ABSL_FLAG(double, k_factor, 32.0, "ELO K factor");
ABSL_FLAG(int, mcts_iterations, 400,
          "Default iterations for builtin:mcts opponents");
ABSL_FLAG(std::string, base_commit, "HEAD",
          "Commit sandbox workers check out before patching a candidate in. "
          "Recorded on each submission so a rating stays attributable to a "
          "known tree");
ABSL_FLAG(std::string, broker_advertise, "",
          "host:port a built bot should dial. Defaults to "
          "localhost:<grpc_port>, which is only right for same-host workers; "
          "set it explicitly once a fleet runs elsewhere");
ABSL_FLAG(int, placement_games, 4,
          "Games per opponent in a new candidate's placement series");
ABSL_FLAG(int, sandbox_build_timeout_s, 1800,
          "Wall-clock limit a worker gives a candidate build");
ABSL_FLAG(int, sandbox_run_timeout_s, 1800,
          "Wall-clock limit a worker gives one order's games");

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

void WaitForShutdownSignal() {
  std::unique_lock lock(g_shutdown_mutex);
  g_shutdown_cv.wait(lock, [] { return g_shutdown_requested; });
}

}  // namespace

auto main(int argc, char **argv) -> int {
  absl::ParseCommandLine(argc, argv);
  absl::InitializeLog();
  absl::SetStderrThreshold(absl::LogSeverityAtLeast::kInfo);

  const std::filesystem::path data_dir = absl::GetFlag(FLAGS_data_dir);
  std::error_code ec;
  std::filesystem::create_directories(data_dir, ec);
  if (ec) {
    LOG(ERROR) << "Cannot create --data_dir " << data_dir << ": "
               << ec.message();
    return 1;
  }

  tournament_broker::SetDefaultMctsIterations(
      absl::GetFlag(FLAGS_mcts_iterations));

  tournament_broker::EloStore elo_store(data_dir / "ratings.pb",
                                        absl::GetFlag(FLAGS_k_factor));
  elo_store.Load();
  tournament_broker::GameHistory history(data_dir / "games");

  tournament_broker::MatchmakerConfig config;
  config.turn_timeout =
      std::chrono::milliseconds(absl::GetFlag(FLAGS_turn_timeout_ms));
  config.game_time_budget =
      std::chrono::milliseconds(absl::GetFlag(FLAGS_game_time_budget_ms));
  config.rendezvous_timeout =
      std::chrono::milliseconds(absl::GetFlag(FLAGS_rendezvous_timeout_ms));
  config.max_moves_per_game = absl::GetFlag(FLAGS_max_moves_per_game);
  config.worker_threads = absl::GetFlag(FLAGS_worker_threads);
  tournament_broker::Matchmaker matchmaker(config, &elo_store, &history);

  tournament_arena::CandidateStore candidates(data_dir / "candidates");
  candidates.Load();

  tournament_arena::SchedulerConfig scheduler_config;
  scheduler_config.broker_target =
      absl::GetFlag(FLAGS_broker_advertise).empty()
          ? "localhost:" + std::to_string(absl::GetFlag(FLAGS_grpc_port))
          : absl::GetFlag(FLAGS_broker_advertise);
  scheduler_config.placement_games = absl::GetFlag(FLAGS_placement_games);
  scheduler_config.build_timeout_s =
      absl::GetFlag(FLAGS_sandbox_build_timeout_s);
  scheduler_config.run_timeout_s = absl::GetFlag(FLAGS_sandbox_run_timeout_s);
  tournament_arena::Scheduler scheduler(scheduler_config, &candidates,
                                        &elo_store);

  tournament_broker::BrokerService service(
      &matchmaker,
      std::chrono::milliseconds(absl::GetFlag(FLAGS_hello_timeout_ms)));
  tournament_arena::ArenaService arena(&candidates, &scheduler, &elo_store,
                                       absl::GetFlag(FLAGS_base_commit));
  tournament_arena::FleetService fleet(&scheduler);

  grpc::ServerBuilder builder;
  builder.AddListeningPort(
      "0.0.0.0:" + std::to_string(absl::GetFlag(FLAGS_grpc_port)),
      grpc::InsecureServerCredentials());
  // Reclaim connections whose peer disappeared without closing the socket. A
  // bot host that is powered off mid-game leaves no FIN behind, so without
  // keepalive its stream stays open until the OS gives up on the TCP
  // connection, which can be hours.
  const int keepalive_ms = absl::GetFlag(FLAGS_keepalive_s) * 1000;
  builder.AddChannelArgument(GRPC_ARG_KEEPALIVE_TIME_MS, keepalive_ms);
  builder.AddChannelArgument(GRPC_ARG_KEEPALIVE_TIMEOUT_MS, 20000);
  builder.AddChannelArgument(GRPC_ARG_KEEPALIVE_PERMIT_WITHOUT_CALLS, 1);
  // Bots idle between games; do not mistake that for a dead connection.
  builder.AddChannelArgument(GRPC_ARG_HTTP2_MIN_RECV_PING_INTERVAL_WITHOUT_DATA_MS,
                             keepalive_ms / 2);
  builder.RegisterService(&service);
  builder.RegisterService(&arena);
  builder.RegisterService(&fleet);
  std::unique_ptr<grpc::Server> server = builder.BuildAndStart();
  if (!server) {
    LOG(ERROR) << "Cannot bind gRPC port " << absl::GetFlag(FLAGS_grpc_port);
    return 1;
  }

  tournament_broker::HttpLeaderboard leaderboard(
      absl::GetFlag(FLAGS_http_port), &elo_store, &history, &candidates);
  if (!leaderboard.Start()) {
    return 1;
  }

  std::signal(SIGINT, OnShutdownSignal);
  std::signal(SIGTERM, OnShutdownSignal);

  LOG(INFO) << "Tournament server: broker + arena + fleet on :"
            << absl::GetFlag(FLAGS_grpc_port) << ", leaderboard on "
            << "http://localhost:" << absl::GetFlag(FLAGS_http_port)
            << ", data dir " << data_dir << ", bots dial "
            << scheduler_config.broker_target << ", "
            << candidates.size() << " candidate(s) loaded";
  WaitForShutdownSignal();
  LOG(INFO) << "Shutting down";

  // Order matters, and the intuitive order is the slow one. Close the broker
  // first: Shutdown() refuses new joins, releases everyone queued, and aborts
  // games in flight, so each of those RPCs ends on its own -- games even get to
  // write their final GameOver and persist their record.
  matchmaker.Shutdown();
  matchmaker.Drain();
  // By now almost nothing is outstanding, so this returns immediately instead
  // of waiting out the grace period. The deadline is only a backstop for
  // stragglers (a connection that never sent its hello, an arena RPC mid-flight);
  // the no-argument Shutdown() would wait for them forever. Cancelled calls
  // surface as OnCancel in the reactor, which always finishes the RPC.
  server->Shutdown(std::chrono::system_clock::now() +
                   std::chrono::seconds(absl::GetFlag(FLAGS_shutdown_grace_s)));
  leaderboard.Stop();
  return 0;
}
