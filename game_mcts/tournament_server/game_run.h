#ifndef RISK_GAME_AI_CPP_TOURNAMENT_SERVER_GAME_RUN_H
#define RISK_GAME_AI_CPP_TOURNAMENT_SERVER_GAME_RUN_H

// One game, as an event-driven state machine instead of a blocking loop on a
// dedicated thread.
//
// Previously each game owned a std::thread that parked in a condition variable
// waiting for the next action. Here Step() advances the game until it must
// wait on a remote player, sends YourTurn, arms a deadline, and returns; the
// game resumes when an action arrives or the deadline fires. Games therefore
// cost an object rather than a thread, and CPU-heavy built-in moves are bounded
// by the worker pool instead of fanning out without limit.
//
// Every transition runs on the game's own Strand, so session_, seats_, gen_
// and the turn bookkeeping are single-threaded by construction and need no
// locking -- the same invariant the old thread-per-game gave, at no thread
// cost. GameSession is therefore unchanged.

#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <random>
#include <string>

#include "cpp/tournament_server/client_handle.h"
#include "cpp/tournament_server/elo_store.h"
#include "cpp/tournament_server/game_history.h"
#include "cpp/tournament_server/game_session.h"
#include "cpp/tournament_server/tournament_broker.pb.h"
#include "cpp/tournament_server/worker_pool.h"

namespace tournament_broker {

struct GameRunConfig {
  std::chrono::milliseconds turn_timeout{10000};
  // Total wall-clock thinking time one seat may spend across a whole game.
  // Zero disables the budget and leaves only the per-turn timeout, which on
  // its own bounds nothing: a slow strategy can burn turn_timeout on every one
  // of thousands of moves.
  std::chrono::milliseconds game_time_budget{0};
  int max_moves_per_game = 50000;
};

// One seat of a game: either a remote client or a built-in strategy.
struct Seat {
  std::string display_name;
  std::shared_ptr<ClientHandle> client;  // null => built-in
  BuiltinFn builtin;
};

class GameRun : public std::enable_shared_from_this<GameRun> {
 public:
  static auto Create(const GameDescriptor &descriptor, GameRunConfig config,
                     std::array<Seat, 2> seats, uint64_t game_counter,
                     EloStore *elo_store, GameHistory *history,
                     WorkerPool *pool, Timer *timer,
                     Task on_finished) -> std::shared_ptr<GameRun>;

  // Posts the opening work. Call exactly once, after Create().
  void Start();

  // Ends the game early (server shutdown). Safe from any thread.
  void Abort(std::string reason);

  auto game_id() const -> const std::string & { return game_id_; }

 private:
  GameRun(const GameDescriptor &descriptor, GameRunConfig config,
          std::array<Seat, 2> seats, uint64_t game_counter, EloStore *elo_store,
          GameHistory *history, WorkerPool *pool, Timer *timer,
          Task on_finished);

  // --- strand only ---
  void Begin();
  void Step();
  void Conclude(GameOutcome outcome, std::string reason);
  auto SendYourTurn(int seat, std::chrono::milliseconds allowed) -> bool;
  void ArmTurnTimer(std::chrono::milliseconds delay);
  void CancelTurnTimer();

  // --- any thread ---
  void WakeStrand();

  const GameDescriptor &descriptor_;  // static registry entry; outlives us
  const GameRunConfig config_;
  EloStore *elo_store_;   // not owned
  GameHistory *history_;  // not owned
  Timer *timer_;          // not owned
  Task on_finished_;

  std::shared_ptr<Strand> strand_;
  std::string game_id_;

  std::array<Seat, 2> seats_;
  std::unique_ptr<GameSession> session_;
  proto::GameRecord record_;
  std::mt19937 gen_;

  // Keeps the game alive between events: once Step() returns, nothing else
  // holds a strong reference until an action or the deadline arrives.
  // Released in Conclude(), which every path reaches.
  std::shared_ptr<GameRun> self_;

  // Strand-only; no locking.
  uint64_t turn_epoch_ = 0;
  Timer::Id turn_timer_ = 0;
  int waiting_seat_ = -1;
  bool concluded_ = false;
  // Thinking time charged per seat, against config_.game_time_budget.
  std::array<std::chrono::milliseconds, 2> time_used_{};
  std::chrono::steady_clock::time_point turn_started_;
  // Whether the pending turn's deadline came from the game budget rather than
  // the per-turn timeout, so a fired deadline reports the right reason.
  bool turn_budget_bound_ = false;
};

}  // namespace tournament_broker

#endif  // RISK_GAME_AI_CPP_TOURNAMENT_SERVER_GAME_RUN_H
