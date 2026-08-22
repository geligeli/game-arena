// Unit tests for the execution primitives every game now runs on.
//
// The properties worth pinning are the ones whose violation shows up as a
// corrupted game rather than a failing assert: a Strand running two tasks at
// once would race the game state it exists to protect, and a Strand freed by
// its own last task would be a use-after-free in the drain loop.

#include "cpp/tournament_server/worker_pool.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "gtest/gtest.h"

namespace tournament_broker {
namespace {

using std::chrono::milliseconds;

TEST(WorkerPoolTest, RunsEverySubmittedTask) {
  WorkerPool pool(4);
  std::atomic<int> ran{0};
  for (int i = 0; i < 500; ++i) {
    pool.Submit([&ran] { ++ran; });
  }
  pool.Stop();  // drains, then joins
  EXPECT_EQ(ran.load(), 500);
}

TEST(WorkerPoolTest, ClampsThreadCountToAtLeastOne) {
  WorkerPool pool(0);
  EXPECT_GE(pool.size(), 1);
  std::atomic<int> ran{0};
  pool.Submit([&ran] { ++ran; });
  pool.Stop();
  EXPECT_EQ(ran.load(), 1);
}

TEST(WorkerPoolTest, StopIsIdempotentAndDropsLaterWork) {
  WorkerPool pool(2);
  pool.Stop();
  std::atomic<int> ran{0};
  pool.Submit([&ran] { ++ran; });
  pool.Stop();
  EXPECT_EQ(ran.load(), 0);
}

// The property the whole design leans on: a game's state needs no mutex
// because its strand never runs two transitions concurrently.
TEST(StrandTest, NeverRunsTwoTasksConcurrently) {
  WorkerPool pool(8);
  auto strand = Strand::Create(&pool);

  std::atomic<int> in_flight{0};
  std::atomic<int> max_in_flight{0};
  std::atomic<int> ran{0};

  for (int i = 0; i < 300; ++i) {
    strand->Post([&] {
      const int now = ++in_flight;
      int previous = max_in_flight.load();
      while (now > previous &&
             !max_in_flight.compare_exchange_weak(previous, now)) {
      }
      std::this_thread::sleep_for(milliseconds(1));
      --in_flight;
      ++ran;
    });
  }
  pool.Stop();

  EXPECT_EQ(ran.load(), 300);
  EXPECT_EQ(max_in_flight.load(), 1) << "strand ran tasks in parallel";
}

TEST(StrandTest, PreservesPostOrder) {
  WorkerPool pool(8);
  auto strand = Strand::Create(&pool);

  std::mutex mu;
  std::vector<int> order;
  for (int i = 0; i < 100; ++i) {
    strand->Post([&, i] {
      std::lock_guard lock(mu);
      order.push_back(i);
    });
  }
  pool.Stop();

  ASSERT_EQ(order.size(), 100u);
  for (int i = 0; i < 100; ++i) {
    EXPECT_EQ(order[static_cast<size_t>(i)], i);
  }
}

// Concurrent posters must not lose tasks or double-schedule the drain.
TEST(StrandTest, SurvivesConcurrentPosters) {
  WorkerPool pool(8);
  auto strand = Strand::Create(&pool);

  std::atomic<int> ran{0};
  std::vector<std::thread> posters;
  posters.reserve(8);
  for (int t = 0; t < 8; ++t) {
    posters.emplace_back([&] {
      for (int i = 0; i < 200; ++i) {
        strand->Post([&ran] { ++ran; });
      }
    });
  }
  for (std::thread &poster : posters) {
    poster.join();
  }
  pool.Stop();
  EXPECT_EQ(ran.load(), 8 * 200);
}

// A game's last strand task drops the reference that owns the game -- and the
// strand with it. The drain loop has to outlive that, which is why Strand is
// shared_ptr owned. Run this under asan to make the regression visible.
TEST(StrandTest, TaskMayDestroyTheStrandsOwner) {
  WorkerPool pool(2);

  struct Owner {
    std::shared_ptr<Strand> strand;
    std::atomic<int> *ran;
  };

  std::atomic<int> ran{0};
  for (int i = 0; i < 50; ++i) {
    auto owner = std::make_shared<Owner>();
    owner->strand = Strand::Create(&pool);
    owner->ran = &ran;
    // The task holds the only reference to the owner, so completing it frees
    // the owner and releases its strand reference mid-drain.
    owner->strand->Post([owner] { ++*owner->ran; });
  }
  pool.Stop();
  EXPECT_EQ(ran.load(), 50);
}

TEST(TimerTest, FiresAfterTheDelay) {
  Timer timer;
  std::atomic<bool> fired{false};
  const auto start = std::chrono::steady_clock::now();
  timer.After(milliseconds(30), [&fired] { fired = true; });

  for (int i = 0; i < 200 && !fired; ++i) {
    std::this_thread::sleep_for(milliseconds(5));
  }
  EXPECT_TRUE(fired);
  EXPECT_GE(std::chrono::steady_clock::now() - start, milliseconds(30));
}

TEST(TimerTest, FiresInDeadlineOrderNotPostOrder) {
  Timer timer;
  std::mutex mu;
  std::vector<int> order;
  timer.After(milliseconds(60), [&] {
    std::lock_guard lock(mu);
    order.push_back(60);
  });
  timer.After(milliseconds(10), [&] {
    std::lock_guard lock(mu);
    order.push_back(10);
  });

  for (int i = 0; i < 200; ++i) {
    {
      std::lock_guard lock(mu);
      if (order.size() == 2) break;
    }
    std::this_thread::sleep_for(milliseconds(5));
  }
  std::lock_guard lock(mu);
  ASSERT_EQ(order.size(), 2u);
  EXPECT_EQ(order[0], 10);
  EXPECT_EQ(order[1], 60);
}

TEST(TimerTest, CancelPreventsAPendingFire) {
  Timer timer;
  std::atomic<bool> fired{false};
  const Timer::Id id = timer.After(milliseconds(30), [&fired] { fired = true; });
  timer.Cancel(id);
  std::this_thread::sleep_for(milliseconds(80));
  EXPECT_FALSE(fired);
}

// Shutdown must not wait out a pending turn timeout.
TEST(TimerTest, StopAbandonsPendingTimers) {
  std::atomic<bool> fired{false};
  {
    Timer timer;
    timer.After(milliseconds(10000), [&fired] { fired = true; });
    const auto start = std::chrono::steady_clock::now();
    timer.Stop();
    EXPECT_LT(std::chrono::steady_clock::now() - start, milliseconds(1000));
  }
  EXPECT_FALSE(fired);
}

}  // namespace
}  // namespace tournament_broker
