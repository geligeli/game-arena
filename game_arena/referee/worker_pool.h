#ifndef GAME_ARENA_GAME_ARENA_REFEREE_WORKER_POOL_H
#define GAME_ARENA_GAME_ARENA_REFEREE_WORKER_POOL_H

// Game execution without gRPC, so the game layer stays transport agnostic.

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
  WorkerPool &operator=(const WorkerPool &) = delete;

  // Queues |task|. Silently dropped after Stop().
  void Submit(Task task);

  // Drains what is already queued, then joins every worker. Idempotent.
  void Stop();

  int size() const { return static_cast<int>(threads_.size()); }

 private:
  void WorkerLoop();

  std::mutex mu_;
  std::condition_variable cv_;
  std::deque<Task> queue_;
  bool stopping_ = false;
  std::vector<std::thread> threads_;
};

// Serial executor over a WorkerPool. shared_ptr owned: a task may destroy the
// strand's owner, so the drain loop keeps the strand alive.
class Strand : public std::enable_shared_from_this<Strand> {
 public:
  static std::shared_ptr<Strand> Create(WorkerPool *pool) {
    return std::shared_ptr<Strand>(new Strand(pool));
  }

  Strand(const Strand &) = delete;
  Strand &operator=(const Strand &) = delete;

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
  Timer &operator=(const Timer &) = delete;

  // |fn| runs on the timer thread, so it must not block.
  Id After(std::chrono::milliseconds delay, Task fn);

  // Best effort: one already being dispatched still runs. O(log n), as every
  // move calls it.
  void Cancel(Id id);

  void Stop();

 private:
  void TimerLoop();

  using Deadline = std::chrono::steady_clock::time_point;

  std::mutex mu_;
  std::condition_variable cv_;
  // Ordered by deadline; |deadlines_| maps an id back to its key for Cancel().
  std::map<std::pair<Deadline, Id>, Task> entries_;
  std::unordered_map<Id, Deadline> deadlines_;
  Id next_id_ = 1;
  bool stopping_ = false;
  std::thread thread_;
};

}  // namespace tournament_broker

#endif  // GAME_ARENA_GAME_ARENA_REFEREE_WORKER_POOL_H
