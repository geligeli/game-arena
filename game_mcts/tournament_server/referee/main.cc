// The match referee: plays a fixed number of games between two named players
// and prints the tally.
/*
bazel run //game_mcts/tournament_server/testgame:match_referee -- \
    --port=50051 --game=nim --games=10 \
    --player_a=fast-a1b2c3 --player_b=builtin:optimal
*/
//
// This is the broker, scoped to one match and given a reason to exit. The
// arena's coordinator used to host it; now a sandbox worker starts one of these
// per order on a private network, points the two bot containers at it, and
// reads the last line of its stdout. That is what lets the coordinator link no
// game code at all -- the rules live here, and here runs on a worker.
//
// The bots are unchanged: they dial in with the same Hello the broker always
// took. Against a builtin, only one bot connects and names
// --player_b=builtin:<spec> as its opponent; against another submission, both
// connect and rendezvous on "player:<the other>".
//
// Output contract, last line, parsed by sandbox/worker/build_log.h:
//
//   RESULT games=10 wins=6 draws=1 losses=3 elo=1523.4
//
// counted from --player_a's side. The elo figure is this match's own, from a
// fresh 1500: the coordinator keeps the authoritative rating and recomputes it
// from the w/d/l above. Reporting it anyway keeps the line identical to the one
// the candidate harness already prints.

#include <grpcpp/grpcpp.h>

#include <atomic>
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
#include "game_mcts/tournament_server/proto/tournament_broker.pb.h"
#include "game_mcts/tournament_server/referee/broker_service.h"
#include "game_mcts/tournament_server/referee/game_registry.h"
#include "game_mcts/tournament_server/referee/matchmaker.h"
#include "game_mcts/tournament_server/server/elo_store.h"
#include "game_mcts/tournament_server/server/game_history.h"

ABSL_FLAG(int, port, 50051, "Port the two sides dial");
ABSL_FLAG(std::string, game, "", "Game registry key (required)");
ABSL_FLAG(int, games, 1, "Games to play before reporting and exiting");
ABSL_FLAG(std::string, player_a, "",
          "Player the tally is counted from (required)");
ABSL_FLAG(std::string, player_b, "",
          "The opponent, for the log only: a builtin plays because the bot "
          "named it, and a rival plays because both bots rendezvous");
ABSL_FLAG(std::string, scratch_dir, "",
          "Where the per-match ELO and game records are written. Both are "
          "thrown away with the container; the coordinator holds the real "
          "ratings. Empty: a temp directory");
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
ABSL_FLAG(int, mcts_iterations, 400,
          "Default iterations for builtin:mcts opponents");
ABSL_FLAG(std::string, port_file, "",
          "Write the bound port here once listening, then the bots can be "
          "started against it. Use with --port=0 to let the OS pick: picking a "
          "free port in the caller and passing it down races with anything "
          "else on the host claiming it in between");

namespace {

// Counts finished games from --player_a's side.
//
// Written from game strands and read by main, so every field is guarded. The
// matchmaker plays games concurrently when both sides reconnect fast enough,
// which is why this is a mutex rather than a plain counter.
class Tally {
 public:
  Tally(std::string player_a, int target)
      : player_a_(std::move(player_a)), target_(target) {}

  void Observe(const tournament_broker::proto::GameRecord &record) {
    int seat = -1;
    for (int i = 0; i < record.player_names_size(); ++i) {
      if (record.player_names(i) == player_a_) {
        seat = i;
        break;
      }
    }
    {
      std::lock_guard lock(mutex_);
      // A game player_a was not in cannot be scored from its side. Nothing
      // should produce one, so say so rather than silently miscounting.
      if (seat < 0) {
        LOG(WARNING) << "Ignoring game " << record.game_id() << ": "
                     << player_a_ << " is not a seat in it";
        return;
      }
      ++games_;
      if (record.result() == tournament_broker::proto::GameRecord::DRAW) {
        ++draws_;
      } else if (record.winning_player() == seat) {
        ++wins_;
      } else {
        ++losses_;
      }
    }
    cv_.notify_all();
  }

  // Blocks until the target is reached or |deadline| passes. Returns true if
  // the full match was played.
  auto Await(std::chrono::steady_clock::time_point deadline) -> bool {
    std::unique_lock lock(mutex_);
    if (deadline == std::chrono::steady_clock::time_point::max()) {
      cv_.wait(lock, [&] { return games_ >= target_; });
      return true;
    }
    return cv_.wait_until(lock, deadline, [&] { return games_ >= target_; });
  }

  struct Counts {
    int games, wins, draws, losses;
  };
  auto counts() const -> Counts {
    std::lock_guard lock(mutex_);
    return {games_, wins_, draws_, losses_};
  }

 private:
  const std::string player_a_;
  const int target_;
  mutable std::mutex mutex_;
  std::condition_variable cv_;
  int games_ = 0, wins_ = 0, draws_ = 0, losses_ = 0;
};

}  // namespace

auto main(int argc, char **argv) -> int {
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
  tournament_broker::SetDefaultMctsIterations(
      absl::GetFlag(FLAGS_mcts_iterations));

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

  // Both are per-match and discarded: the coordinator owns the real standings.
  // They exist because a game insists on somewhere to record itself, and a
  // referee that dropped its records on the floor would be harder to debug.
  tournament_broker::EloStore elo_store(scratch / "ratings.pb");
  elo_store.Load();
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
  config.on_record = [&tally](const tournament_broker::proto::GameRecord &r) {
    tally.Observe(r);
  };
  tournament_broker::Matchmaker matchmaker(config, &elo_store, &history);
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

  // Written after the listener is up, so a worker that sees the file knows the
  // port accepts connections. Written to a temp name and renamed, so a reader
  // never sees a half-written number.
  const std::string port_file = absl::GetFlag(FLAGS_port_file);
  if (!port_file.empty()) {
    const std::filesystem::path tmp = port_file + ".tmp";
    {
      std::ofstream out(tmp);
      out << bound_port << "\n";
      if (!out) {
        LOG(ERROR) << "Cannot write --port_file " << tmp;
        return 1;
      }
    }
    std::filesystem::rename(tmp, port_file, ec);
    if (ec) {
      LOG(ERROR) << "Cannot rename " << tmp << " to " << port_file << ": "
                 << ec.message();
      return 1;
    }
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

  // Shutdown before Drain, or Drain waits out a full turn timeout for every
  // game parked on a client that will never answer.
  matchmaker.Shutdown();
  matchmaker.Drain();
  server->Shutdown(std::chrono::system_clock::now() + std::chrono::seconds(5));

  const Tally::Counts counts = tally.counts();
  if (!complete) {
    LOG(ERROR) << "Deadline reached after " << counts.games << " of "
               << target_games << " games";
  }
  // Last line, and the whole point of the process. Printed even on a partial
  // match: the games that were played are real results, and the worker can
  // tell the match was short because games < the number it asked for.
  std::printf("RESULT games=%d wins=%d draws=%d losses=%d elo=%.1f\n",
              counts.games, counts.wins, counts.draws, counts.losses,
              elo_store.Get(game, player_a).elo());
  std::fflush(stdout);
  return complete ? 0 : 3;
}
