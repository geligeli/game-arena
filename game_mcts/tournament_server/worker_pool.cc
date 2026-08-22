#include "cpp/tournament_server/worker_pool.h"

#include <algorithm>
#include <utility>

namespace tournament_broker {

WorkerPool::WorkerPool(int num_threads) {
  const int count = std::max(1, num_threads);
  threads_.reserve(static_cast<size_t>(count));
  for (int i = 0; i < count; ++i) {
    threads_.emplace_back([this] { WorkerLoop(); });
  }
}

WorkerPool::~WorkerPool() { Stop(); }

void WorkerPool::Submit(Task task) {
  {
    std::lock_guard lock(mu_);
    if (stopping_) {
      return;
    }
    queue_.push_back(std::move(task));
  }
  cv_.notify_one();
}

void WorkerPool::Stop() {
  {
    std::lock_guard lock(mu_);
    if (stopping_) {
      return;
    }
    stopping_ = true;
  }
  cv_.notify_all();
  for (std::thread &thread : threads_) {
    if (thread.joinable()) {
      thread.join();
    }
  }
  threads_.clear();
}

void WorkerPool::WorkerLoop() {
  for (;;) {
    Task task;
    {
      std::unique_lock lock(mu_);
      cv_.wait(lock, [&] { return stopping_ || !queue_.empty(); });
      // Drain what is already queued even while stopping, so a game that has
      // reached its persistence step still finishes writing.
      if (queue_.empty()) {
        return;
      }
      task = std::move(queue_.front());
      queue_.pop_front();
    }
    std::move(task)();
  }
}

void Strand::Post(Task task) {
  bool schedule = false;
  {
    std::lock_guard lock(mu_);
    queue_.push_back(std::move(task));
    if (!draining_) {
      draining_ = true;
      schedule = true;
    }
  }
  if (schedule) {
    // Holds the strand alive for the whole drain: the last task may destroy
    // whatever owns this strand.
    pool_->Submit([self = shared_from_this()] { self->Drain(); });
  }
}

void Strand::Drain() {
  for (;;) {
    Task task;
    {
      std::lock_guard lock(mu_);
      if (queue_.empty()) {
        draining_ = false;
        return;
      }
      task = std::move(queue_.front());
      queue_.pop_front();
    }
    // Never held while running: a task may Post() back onto this strand.
    std::move(task)();
  }
}

Timer::Timer() : thread_([this] { TimerLoop(); }) {}

Timer::~Timer() { Stop(); }

auto Timer::After(std::chrono::milliseconds delay, Task fn) -> Id {
  Id id = 0;
  {
    std::lock_guard lock(mu_);
    if (stopping_) {
      return 0;
    }
    id = next_id_++;
    const Deadline deadline = std::chrono::steady_clock::now() + delay;
    entries_.emplace(std::make_pair(deadline, id), std::move(fn));
    deadlines_.emplace(id, deadline);
  }
  cv_.notify_one();
  return id;
}

void Timer::Cancel(Id id) {
  if (id == 0) {
    return;
  }
  std::lock_guard lock(mu_);
  const auto found = deadlines_.find(id);
  if (found == deadlines_.end()) {
    return;  // Already fired or never armed.
  }
  entries_.erase(std::make_pair(found->second, id));
  deadlines_.erase(found);
}

void Timer::Stop() {
  {
    std::lock_guard lock(mu_);
    if (stopping_) {
      return;
    }
    stopping_ = true;
    // Pending deadlines are abandoned, not run: shutdown must not wait out a
    // turn timeout.
    entries_.clear();
    deadlines_.clear();
  }
  cv_.notify_all();
  if (thread_.joinable()) {
    thread_.join();
  }
}

void Timer::TimerLoop() {
  for (;;) {
    Task due;
    {
      std::unique_lock lock(mu_);
      if (stopping_) {
        return;
      }
      if (entries_.empty()) {
        cv_.wait(lock, [&] { return stopping_ || !entries_.empty(); });
        continue;
      }
      const auto next = entries_.begin()->first.first;
      if (std::chrono::steady_clock::now() < next) {
        cv_.wait_until(lock, next);
        continue;  // Re-check: an earlier entry may have arrived.
      }
      due = std::move(entries_.begin()->second);
      deadlines_.erase(entries_.begin()->first.second);
      entries_.erase(entries_.begin());
    }
    std::move(due)();
  }
}

}  // namespace tournament_broker
