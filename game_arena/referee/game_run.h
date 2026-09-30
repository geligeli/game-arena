#ifndef GAME_ARENA_GAME_ARENA_REFEREE_GAME_RUN_H
#define GAME_ARENA_GAME_ARENA_REFEREE_GAME_RUN_H

// One game as a state machine: every transition runs on its own Strand, so its
// state needs no locks.

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "game_arena/proto/tournament_broker.pb.h"
#include "game_arena/referee/client_handle.h"
#include "game_arena/referee/game_session.h"
#include "game_arena/referee/worker_pool.h"
#include "game_arena/standings/game_history.h"

namespace tournament_broker {

struct GameRunConfig {
  std::chrono::milliseconds turn_timeout{10000};
  // Per seat per game; 0 leaves only turn_timeout, which alone bounds nothing.
  std::chrono::milliseconds game_time_budget{0};
  int max_moves_per_game = 50000;

  // On the game's strand, right after the record is stored.
  std::function<void(const proto::GameRecord&)> on_record;
};

struct Seat {
  std::string display_name;
  std::shared_ptr<ClientHandle> client;  // null => built-in
  BuiltinFn builtin;
};

class GameRun : public std::enable_shared_from_this<GameRun> {
 public:
  // Construct through std::make_shared: Start() needs shared_from_this().
  GameRun(const GameDescriptor& descriptor, GameRunConfig config,
          std::vector<Seat> seats, uint64_t game_counter, GameHistory* history,
          WorkerPool* pool, Timer* timer, Task on_finished);

  // Posts the opening work. Call exactly once.
  void Start();

  // Ends the game early (server shutdown). Safe from any thread.
  void Abort(std::string reason);

 private:
  // --- strand only ---
  void Begin();
  void Step();
  // The seat places below everyone still playing and a builtin plays on for
  // it. False, the game over, once one seat is left: that one wins.
  bool Forfeit(int seat, std::string reason);
  // |places| as the game ranks the seats; forfeiters go below them, the
  // latest first, and with |tiebreak| level seats go by thinking time.
  void Conclude(std::vector<int> places, std::string reason, bool tiebreak);
  void CaptureViews();
  bool SendYourTurn(int seat, std::chrono::milliseconds allowed);
  void ArmTurnTimer(std::chrono::milliseconds delay);
  void CancelTurnTimer();

  // --- any thread ---
  void WakeStrand();

  const GameDescriptor& descriptor_;  // static registry entry; outlives us
  const GameRunConfig config_;
  GameHistory* history_;  // not owned
  Timer* timer_;          // not owned
  Task on_finished_;

  std::shared_ptr<Strand> strand_;
  std::string game_id_;

  std::vector<Seat> seats_;
  // By seat, a forfeiter's too: its GameOver waits for the game to end.
  std::vector<std::shared_ptr<ClientHandle>> clients_;
  std::unique_ptr<GameSession> session_;
  proto::GameRecord record_;
  std::mt19937 gen_;
  struct Captured {
    std::string caption;
    std::string view;
  };
  std::vector<Captured> captured_;

  // Keeps the game alive between events; released in Conclude().
  std::shared_ptr<GameRun> self_;

  // Strand-only; no locking.
  uint64_t turn_epoch_ = 0;
  Timer::Id turn_timer_ = 0;
  int waiting_seat_ = -1;
  bool concluded_ = false;
  std::vector<std::chrono::steady_clock::duration> time_used_;
  std::vector<int> forfeited_;  // seats, in the order they forfeited
  std::chrono::steady_clock::time_point turn_started_;
  // The pending deadline is the game budget's rather than turn_timeout's.
  bool turn_budget_bound_ = false;
};

}  // namespace tournament_broker

#endif  // GAME_ARENA_GAME_ARENA_REFEREE_GAME_RUN_H
