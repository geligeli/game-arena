// One match, then exit: a MatchReport at --report, and a last stdout line
// "RESULT games=10 wins=6 draws=1 losses=3" counted from --player_a's side.

#include <grpcpp/grpcpp.h>

#include <chrono>
#include <condition_variable>
#include <cstdio>
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
#include "game_arena/common/kv_options/kv_options.h"
#include "game_arena/proto/tournament_broker.pb.h"
#include "game_arena/referee/game_registry.h"
#include "game_arena/referee/match_tally.h"
#include "game_arena/referee/matchmaker.h"
#include "game_arena/referee/play_reactor.h"
#include "game_arena/standings/game_history.h"

ABSL_FLAG(int, port, 50051, "Port the two sides dial");
ABSL_FLAG(std::string, game, "", "Game registry key (required)");
ABSL_FLAG(int, games, 1, "Games to play before reporting and exiting");
ABSL_FLAG(std::string, player_a, "",
          "Player the tally is counted from (required)");
ABSL_FLAG(std::string, player_b, "",
          "The opponent, for the log only: a builtin plays because the bot "
          "named it, and a rival plays because both bots rendezvous");
ABSL_FLAG(std::string, report, "",
          "Write the MatchReport here before exiting. Empty: none");
ABSL_FLAG(std::string, scratch_dir, "",
          "Where the game records are written. Empty: a temp directory");
ABSL_FLAG(int, turn_timeout_ms, 10000,
          "Per-turn wall-clock limit; exceeding it loses the game");
ABSL_FLAG(int, game_time_budget_ms, 0,
          "Total thinking time per seat per game. 0 leaves only "
          "--turn_timeout_ms, which on its own bounds nothing");
ABSL_FLAG(int, rendezvous_timeout_ms, 60000,
          "How long one side waits for its named partner before giving up");
ABSL_FLAG(int, max_moves_per_game, 50000,
          "Safety cap on moves per game before declaring a draw");
ABSL_FLAG(int, worker_threads, 0,
          "Threads serving games. 0 uses hardware_concurrency()");
ABSL_FLAG(int, deadline_s, 0,
          "Give up and report what was played after this long. 0 waits "
          "forever, leaving the worker's own timeout as the only bound");
ABSL_FLAG(std::string, registry_options, "",
          "Comma-separated key=value settings for the linked game registry, "
          "e.g. \"mcts_iterations=400\". Unknown keys are ignored");
ABSL_FLAG(std::string, port_file, "",
          "Write the bound port here once listening, then the bots can be "
          "started against it. Use with --port=0 to let the OS pick: picking a "
          "free port in the caller and passing it down races with anything "
          "else on the host claiming it in between");

namespace {

// Written from game strands, which may run concurrently, and read by main.
class Tally {
 public:
  Tally(std::string player_a, int target)
      : player_a_(std::move(player_a)), target_(target) {}

  void Observe(const tournament_broker::proto::GameRecord& record) {
    {
      std::lock_guard lock(mutex_);
      if (!tournament_broker::AddGame(record, player_a_, &counts_)) {
        LOG(WARNING) << "Ignoring game " << record.game_id() << ": "
                     << player_a_ << " is not a seat in it";
        return;
      }
      *report_.add_games() = record;
    }
    cv_.notify_all();
  }

  // True once the full match is played; false if |deadline| passes first.
  bool Await(std::chrono::steady_clock::time_point deadline) {
    std::unique_lock lock(mutex_);
    if (deadline == std::chrono::steady_clock::time_point::max()) {
      cv_.wait(lock, [&] { return counts_.games >= target_; });
      return true;
    }
    return cv_.wait_until(lock, deadline,
                          [&] { return counts_.games >= target_; });
  }

  tournament_broker::MatchTally counts() const {
    std::lock_guard lock(mutex_);
    return counts_;
  }
  tournament_broker::proto::MatchReport report() const {
    std::lock_guard lock(mutex_);
    return report_;
  }

 private:
  const std::string player_a_;
  const int target_;
  mutable std::mutex mutex_;
  std::condition_variable cv_;
  tournament_broker::MatchTally counts_;
  tournament_broker::proto::MatchReport report_;
};

}  // namespace

int main(int argc, char** argv) {
  absl::ParseCommandLine(argc, argv);
  absl::InitializeLog();
  absl::SetStderrThreshold(absl::LogSeverityAtLeast::kInfo);

  const std::string game = absl::GetFlag(FLAGS_game);
  const std::string player_a = absl::GetFlag(FLAGS_player_a);
  const int target_games = absl::GetFlag(FLAGS_games);
  if (game.empty() || player_a.empty() || target_games <= 0) {
    LOG(ERROR) << "--game, --player_a and a positive --games are required";
    return 2;
  }
  if (!tournament_broker::GameRegistry().contains(game)) {
    LOG(ERROR) << "Unknown --game " << game;
    return 2;
  }
  tournament_broker::SetRegistryOptions(
      kv_options::Parse(absl::GetFlag(FLAGS_registry_options)));

  std::filesystem::path scratch = absl::GetFlag(FLAGS_scratch_dir);
  if (scratch.empty()) {
    scratch = std::filesystem::temp_directory_path() /
              ("match_referee_" + std::to_string(::getpid()));
  }
  std::error_code ec;
  std::filesystem::create_directories(scratch, ec);
  if (ec) {
    LOG(ERROR) << "Cannot create --scratch_dir " << scratch << ": "
               << ec.message();
    return 1;
  }

  tournament_broker::GameHistory history(scratch / "games");

  Tally tally(player_a, target_games);

  tournament_broker::MatchmakerConfig config;
  config.turn_timeout =
      std::chrono::milliseconds(absl::GetFlag(FLAGS_turn_timeout_ms));
  config.game_time_budget =
      std::chrono::milliseconds(absl::GetFlag(FLAGS_game_time_budget_ms));
  config.rendezvous_timeout =
      std::chrono::milliseconds(absl::GetFlag(FLAGS_rendezvous_timeout_ms));
  config.max_moves_per_game = absl::GetFlag(FLAGS_max_moves_per_game);
  config.worker_threads = absl::GetFlag(FLAGS_worker_threads);
  config.on_record = [&tally](const tournament_broker::proto::GameRecord& r) {
    tally.Observe(r);
  };
  tournament_broker::Matchmaker matchmaker(config, &history);
  tournament_broker::BrokerService service(&matchmaker);

  grpc::ServerBuilder builder;
  int bound_port = 0;
  builder.AddListeningPort(
      "0.0.0.0:" + std::to_string(absl::GetFlag(FLAGS_port)),
      grpc::InsecureServerCredentials(), &bound_port);
  builder.RegisterService(&service);
  std::unique_ptr<grpc::Server> server = builder.BuildAndStart();
  if (!server) {
    LOG(ERROR) << "Cannot bind port " << absl::GetFlag(FLAGS_port);
    return 1;
  }

  // Written once the listener is up: a worker that sees the file can connect.
  const std::string port_file = absl::GetFlag(FLAGS_port_file);
  std::string error;
  if (!port_file.empty() &&
      !tournament_broker::WriteAtomically(
          port_file, std::to_string(bound_port) + "\n", &error)) {
    LOG(ERROR) << "Cannot write --port_file: " << error;
    return 1;
  }

  LOG(INFO) << "Referee on :" << bound_port << " for " << target_games << " "
            << game << " game(s): " << player_a << " vs "
            << (absl::GetFlag(FLAGS_player_b).empty()
                    ? "(whoever dials in)"
                    : absl::GetFlag(FLAGS_player_b));

  const int deadline_s = absl::GetFlag(FLAGS_deadline_s);
  const auto deadline =
      deadline_s > 0
          ? std::chrono::steady_clock::now() + std::chrono::seconds(deadline_s)
          : std::chrono::steady_clock::time_point::max();
  const bool complete = tally.Await(deadline);

  // Shutdown first, or Drain waits out a turn timeout per silent client.
  matchmaker.Shutdown();
  matchmaker.Drain();
  server->Shutdown(std::chrono::system_clock::now() + std::chrono::seconds(5));

  const tournament_broker::MatchTally counts = tally.counts();
  if (!complete) {
    LOG(ERROR) << "Deadline reached after " << counts.games << " of "
               << target_games << " games";
  }
  // Even for a partial match: the worker sees fewer games than it asked for.
  const std::string report = absl::GetFlag(FLAGS_report);
  if (!report.empty()) {
    std::ofstream out(report, std::ios::binary);
    if (!tally.report().SerializeToOstream(&out)) {
      LOG(ERROR) << "Cannot write --report " << report;
      return 1;
    }
  }
  std::printf("RESULT games=%d wins=%d draws=%d losses=%d\n", counts.games,
              counts.wins, counts.draws, counts.losses);
  std::fflush(stdout);
  return complete ? 0 : 3;
}
