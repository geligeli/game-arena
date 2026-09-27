#ifndef GAME_ARENA_GAME_ARENA_REFEREE_MATCHMAKER_H
#define GAME_ARENA_GAME_ARENA_REFEREE_MATCHMAKER_H

// Pairs a Hello with its opponent ("builtin:<spec>" or "player:<name>") and
// runs their games as GameRuns on a shared pool.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "game_arena/proto/tournament_broker.pb.h"
#include "game_arena/referee/client_handle.h"
#include "game_arena/referee/game_run.h"
#include "game_arena/referee/game_session.h"
#include "game_arena/referee/worker_pool.h"
#include "game_arena/standings/game_history.h"

namespace tournament_broker {

struct MatchmakerConfig {
  std::chrono::milliseconds turn_timeout{10000};
  std::chrono::milliseconds game_time_budget{0};  // see GameRunConfig
  // Or a partner that never builds parks the other side forever.
  std::chrono::milliseconds rendezvous_timeout{60000};
  int max_moves_per_game = 50000;
  std::size_t max_view_bytes = 1 << 20;  // see GameRunConfig
  int worker_threads = 0;                // <= 0: hardware_concurrency()

  std::function<void(const proto::GameRecord&)> on_record;
};

class Matchmaker {
 public:
  Matchmaker(MatchmakerConfig config, GameHistory* history);
  ~Matchmaker();

  Matchmaker(const Matchmaker&) = delete;
  Matchmaker& operator=(const Matchmaker&) = delete;

  // Never blocks. False, with *error set, when the game, builtin spec or
  // partner name is unusable.
  bool Join(std::shared_ptr<ClientHandle> client, const proto::Hello& hello,
            std::string* error);

  // Unparks the client, and marks it disconnected so its game is forfeited.
  void Disconnect(const std::shared_ptr<ClientHandle>& client);

  // Refuses joins, releases the waiting and aborts running games. Idempotent.
  void Shutdown();

  // Blocks until all running games finish; Shutdown() first bounds it.
  void Drain();

 private:
  struct Parked {
    std::shared_ptr<ClientHandle> client;
    std::string game;
    std::chrono::steady_clock::time_point deadline;
  };

  // Seat 0 moves first.
  void StartGame(const GameDescriptor& descriptor, Seat seat0, Seat seat1);
  // Precondition: |wanted| is non-empty and not the client's own name.
  bool JoinRendezvous(std::shared_ptr<ClientHandle> client,
                      const std::string& game, const std::string& wanted,
                      std::string* error);
  void ReaperLoop();

  const MatchmakerConfig config_;
  GameHistory* history_;  // not owned

  mutable std::mutex mutex_;  // guards rendezvous_*, running_, stopping_
  // By RendezvousKey(), so a collision means each side named the other.
  std::map<std::string, Parked> rendezvous_;
  // Games played per key, so a pair alternates seats whoever parks first.
  std::map<std::string, uint64_t> rendezvous_games_;
  // Weak: only for Shutdown() to reach games, never to keep one alive.
  std::map<uint64_t, std::weak_ptr<GameRun>> running_;
  bool stopping_ = false;
  std::condition_variable reaper_cv_;

  std::atomic<int> running_games_{0};
  std::atomic<uint64_t> game_counter_{0};
  std::mutex drain_mutex_;
  std::condition_variable drain_cv_;

  // Declared before reaper_ so they outlive it.
  WorkerPool pool_;
  Timer timer_;
  std::thread reaper_;
};

}  // namespace tournament_broker

#endif  // GAME_ARENA_GAME_ARENA_REFEREE_MATCHMAKER_H
