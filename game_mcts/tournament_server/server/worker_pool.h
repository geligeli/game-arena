#ifndef RISK_GAME_AI_CPP_TOURNAMENT_SERVER_WORKER_POOL_H
#define RISK_GAME_AI_CPP_TOURNAMENT_SERVER_WORKER_POOL_H

// Bounded execution for games, replacing one detached std::thread per game.
//
//   WorkerPool  fixed set of threads draining a task queue. Bounds how much
//               CPU-heavy built-in work (MCTS, minimax) runs at once, which
//               used to be unbounded: N concurrent games meant N threads
//               fighting for cores.
//   Strand      serializes tasks posted to it without owning a thread. A game
//               puts every state transition on its own strand, so the whole
//               state machine is single-threaded by construction and needs no
//               locking of its own.
//   Timer       one thread serving all deadlines. Kept here rather than using
//               grpc::Alarm so the game layer stays free of gRPC (and clear of
//               Alarm's "not reusable while armed" CHECK).
//
// Deliberately no gRPC dependency: the matchmaker and game runner stay
// transport agnostic and unit-testable without a server.

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "absl/functional/any_invocable.h"

namespace tournament_broker {

using Task = absl::AnyInvocable<void() &&>;

class WorkerPool {
 public:
  // |num_threads| < 1 is clamped to 1.
  explicit WorkerPool(int num_threads);
  ~WorkerPool();

  WorkerPool(const WorkerPool &) = delete;
  auto operator=(const WorkerPool &) -> WorkerPool & = delete;

  // Queues |task|. Silently dropped after Stop().
  void Submit(Task task);

  // Drains what is already queued, then joins every worker. Idempotent.
  void Stop();

  auto size() const -> int { return static_cast<int>(threads_.size()); }

 private:
  void WorkerLoop();

  std::mutex mu_;
  std::condition_variable cv_;
  std::deque<Task> queue_;
  bool stopping_ = false;
  std::vector<std::thread> threads_;
};

// Serial executor over a WorkerPool. Tasks posted to one Strand never run
// concurrently with each other, but a Strand costs no thread of its own.
//
// shared_ptr owned on purpose: a task typically holds the only reference to
// the object that owns this strand, so finishing that task can destroy the
// owner. The drain loop keeps itself alive across that, instead of returning
// into a freed strand.
class Strand : public std::enable_shared_from_this<Strand> {
 public:
  static auto Create(WorkerPool *pool) -> std::shared_ptr<Strand> {
    return std::shared_ptr<Strand>(new Strand(pool));
  }

  Strand(const Strand &) = delete;
  auto operator=(const Strand &) -> Strand & = delete;

  void Post(Task task);

 private:
  explicit Strand(WorkerPool *pool) : pool_(pool) {}

  void Drain();

  WorkerPool *pool_;  // not owned
  std::mutex mu_;
  std::deque<Task> queue_;
  bool draining_ = false;  // a drain task is already queued on the pool
};

class Timer {
 public:
  using Id = uint64_t;

  Timer();
  ~Timer();

  Timer(const Timer &) = delete;
  auto operator=(const Timer &) -> Timer & = delete;

  // Runs |fn| on the timer thread after |delay|. |fn| must not block: post the
  // real work elsewhere. Returns an id usable with Cancel().
  auto After(std::chrono::milliseconds delay, Task fn) -> Id;

  // Best-effort: a timer already being dispatched still runs. Callers must
  // therefore tolerate a late fire (games compare a turn epoch). O(log n) --
  // every move arms and cancels one of these, so a linear scan here is
  // quadratic in the number of concurrent games.
  void Cancel(Id id);

  void Stop();

 private:
  void TimerLoop();

  using Deadline = std::chrono::steady_clock::time_point;

  std::mutex mu_;
  std::condition_variable cv_;
  // Keyed by (deadline, id) so ordering is still by deadline while every entry
  // is directly addressable, and |deadlines_| maps an id back to its key.
  // Cancel is then a lookup rather than a walk.
  std::map<std::pair<Deadline, Id>, Task> entries_;
  std::unordered_map<Id, Deadline> deadlines_;
  Id next_id_ = 1;
  bool stopping_ = false;
  std::thread thread_;
};

}  // namespace tournament_broker

#endif  // RISK_GAME_AI_CPP_TOURNAMENT_SERVER_WORKER_POOL_H
