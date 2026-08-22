// The scheduler, driven by fake workers so neither gRPC nor a real bazel build
// is in the loop.
//
// The behaviour that matters most here is batch atomicity: a
// candidate-vs-candidate match is two orders that must be dispatched together,
// because a lone half would sit at the broker's rendezvous until it timed out,
// holding a slot and producing no game.

#include "cpp/tournament_server/scheduler.h"

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "gtest/gtest.h"

#include <unistd.h>

namespace tournament_arena {
namespace {

class FakeWorker : public FleetWorker {
 public:
  FakeWorker(std::string id, int slots)
      : id_(std::move(id)), slots_(slots) {}

  auto worker_id() const -> std::string override { return id_; }
  auto slots() const -> int override { return slots_; }

  auto Send(const proto::FleetMessage &msg) -> bool override {
    if (!alive_) {
      return false;
    }
    if (msg.has_order()) {
      orders.push_back(msg.order());
    } else if (msg.has_cancel()) {
      cancels.push_back(msg.cancel().order_id());
    }
    return true;
  }

  void Kill() { alive_ = false; }

  std::vector<proto::WorkOrder> orders;
  std::vector<std::string> cancels;

 private:
  std::string id_;
  int slots_;
  bool alive_ = true;
};

class SchedulerTest : public ::testing::Test {
 protected:
  void SetUp() override {
    dir_ = std::filesystem::temp_directory_path() /
           ("scheduler_" + std::to_string(::getpid()) + "_" +
            std::to_string(
                ::testing::UnitTest::GetInstance()->random_seed()));
    std::filesystem::remove_all(dir_);
    store_ = std::make_unique<CandidateStore>(dir_ / "candidates");
    elo_ = std::make_unique<tournament_broker::EloStore>(dir_ / "ratings.pb",
                                                         32.0);
    SchedulerConfig config;
    config.placement_opponents = {"builtin:random"};
    config.placement_games = 2;
    scheduler_ =
        std::make_unique<Scheduler>(config, store_.get(), elo_.get());
  }

  void TearDown() override { std::filesystem::remove_all(dir_); }

  auto AddCandidate(const std::string &name,
                    proto::Candidate::Status status = proto::Candidate::READY)
      -> proto::Candidate {
    proto::SubmitRequest request;
    request.set_display_name(name);
    request.set_author("agent");
    request.set_game("risk2");
    request.set_entry_header("strategy.h");
    auto *file = request.add_files();
    file->set_path("strategy.h");
    file->set_content("// " + name + "\n");
    std::string error;
    const auto candidate = store_->Create(request, "commit0", &error);
    EXPECT_TRUE(candidate.has_value()) << error;
    store_->SetStatus(candidate->candidate_id(), status, "");
    auto updated = store_->Get(candidate->candidate_id());
    return *updated;
  }

  auto Result(const std::string &order_id, bool build_ok = true, int wins = 1,
              int losses = 1) -> proto::OrderResult {
    proto::OrderResult result;
    result.set_order_id(order_id);
    result.set_build_ok(build_ok);
    if (!build_ok) {
      result.set_build_log("strategy.h:12:3: error: no matching function");
      return result;
    }
    result.set_games_played(wins + losses);
    result.set_wins(wins);
    result.set_losses(losses);
    result.set_elo(1520.0);
    return result;
  }

  std::filesystem::path dir_;
  std::unique_ptr<CandidateStore> store_;
  std::unique_ptr<tournament_broker::EloStore> elo_;
  std::unique_ptr<Scheduler> scheduler_;
};

TEST_F(SchedulerTest, PlacementDispatchesOneOrderPerBuiltin) {
  const auto candidate = AddCandidate("Alpha", proto::Candidate::PENDING);
  auto worker = std::make_shared<FakeWorker>("w1", 2);
  scheduler_->AddWorker(worker);

  const std::string job_id = scheduler_->EnqueuePlacement(candidate);
  ASSERT_EQ(worker->orders.size(), 1u);
  const proto::WorkOrder &order = worker->orders[0];
  EXPECT_EQ(order.candidate_id(), candidate.candidate_id());
  EXPECT_EQ(order.opponent(), "builtin:random");
  EXPECT_EQ(order.num_games(), 2);
  EXPECT_EQ(order.base_commit(), "commit0");
  EXPECT_EQ(order.entry_header(), "strategy.h");
  // Sources ride along, so a worker needs nothing but the repo and the order.
  ASSERT_EQ(order.files_size(), 1);
  EXPECT_EQ(order.files(0).path(), "strategy.h");
  EXPECT_FALSE(order.files(0).content().empty());

  scheduler_->OnResult("w1", Result(order.order_id(), true, 2, 0));
  const auto job = scheduler_->GetJob(job_id);
  ASSERT_TRUE(job.has_value());
  EXPECT_EQ(job->state(), proto::Job::DONE);
  EXPECT_EQ(job->wins(), 2);
  EXPECT_EQ(job->games_played(), 2);
  EXPECT_EQ(store_->Get(candidate.candidate_id())->status(),
            proto::Candidate::READY);
}

TEST_F(SchedulerTest, CandidateMatchDispatchesBothSidesNamingEachOther) {
  const auto alpha = AddCandidate("Alpha");
  const auto beta = AddCandidate("Beta");
  auto worker = std::make_shared<FakeWorker>("w1", 4);
  scheduler_->AddWorker(worker);

  std::string error;
  const auto job_id = scheduler_->EnqueueChallenge(
      alpha.candidate_id(), beta.candidate_id(), 6, &error);
  ASSERT_TRUE(job_id.has_value()) << error;

  ASSERT_EQ(worker->orders.size(), 2u);
  const auto &first = worker->orders[0];
  const auto &second = worker->orders[1];
  EXPECT_EQ(first.candidate_id(), alpha.candidate_id());
  EXPECT_EQ(first.opponent(), "player:" + beta.candidate_id());
  EXPECT_EQ(second.candidate_id(), beta.candidate_id());
  EXPECT_EQ(second.opponent(), "player:" + alpha.candidate_id());
  EXPECT_EQ(first.num_games(), 6);
  EXPECT_EQ(second.num_games(), 6);

  // Only the challenger's result counts toward the job's tally; the opponent's
  // order exists to make the game happen.
  scheduler_->OnResult("w1", Result(second.order_id(), true, 2, 4));
  EXPECT_EQ(scheduler_->GetJob(*job_id)->state(), proto::Job::RUNNING);
  scheduler_->OnResult("w1", Result(first.order_id(), true, 4, 2));

  const auto job = scheduler_->GetJob(*job_id);
  ASSERT_TRUE(job.has_value());
  EXPECT_EQ(job->state(), proto::Job::DONE);
  EXPECT_EQ(job->wins(), 4);
  EXPECT_EQ(job->losses(), 2);
  EXPECT_EQ(job->games_played(), 6);
}

// The core invariant: never dispatch half a pair.
TEST_F(SchedulerTest, PairedBatchWaitsForTwoFreeSlots) {
  const auto alpha = AddCandidate("Alpha");
  const auto beta = AddCandidate("Beta");
  auto small = std::make_shared<FakeWorker>("w1", 1);
  scheduler_->AddWorker(small);

  std::string error;
  const auto job_id = scheduler_->EnqueueChallenge(
      alpha.candidate_id(), beta.candidate_id(), 2, &error);
  ASSERT_TRUE(job_id.has_value()) << error;

  // One slot is not enough for a two-order batch, so nothing goes out.
  EXPECT_TRUE(small->orders.empty());
  EXPECT_EQ(scheduler_->queued_batches(), 1);
  EXPECT_EQ(scheduler_->in_flight_orders(), 0);

  // A second host attaching completes the pair, and the batch spreads across
  // both rather than piling onto one.
  auto other = std::make_shared<FakeWorker>("w2", 1);
  scheduler_->AddWorker(other);
  EXPECT_EQ(small->orders.size(), 1u);
  EXPECT_EQ(other->orders.size(), 1u);
  EXPECT_EQ(scheduler_->in_flight_orders(), 2);
  EXPECT_EQ(scheduler_->queued_batches(), 0);
}

TEST_F(SchedulerTest, BatchesQueueUntilAWorkerAttaches) {
  const auto candidate = AddCandidate("Alpha", proto::Candidate::PENDING);
  const std::string job_id = scheduler_->EnqueuePlacement(candidate);
  EXPECT_EQ(scheduler_->queued_batches(), 1);
  EXPECT_EQ(scheduler_->GetJob(job_id)->state(), proto::Job::QUEUED);

  auto worker = std::make_shared<FakeWorker>("w1", 1);
  scheduler_->AddWorker(worker);
  EXPECT_EQ(worker->orders.size(), 1u);
  EXPECT_EQ(scheduler_->GetJob(job_id)->state(), proto::Job::RUNNING);
}

TEST_F(SchedulerTest, BuildFailureFailsTheJobAndMarksTheCandidate) {
  const auto candidate = AddCandidate("Broken", proto::Candidate::PENDING);
  auto worker = std::make_shared<FakeWorker>("w1", 2);
  scheduler_->AddWorker(worker);

  SchedulerConfig config;
  config.placement_opponents = {"builtin:random", "builtin:mcts"};
  config.placement_games = 2;
  Scheduler scheduler(config, store_.get(), elo_.get());
  scheduler.AddWorker(worker);
  const std::string job_id = scheduler.EnqueuePlacement(candidate);
  ASSERT_GE(worker->orders.size(), 1u);

  scheduler.OnResult("w1", Result(worker->orders[0].order_id(),
                                  /*build_ok=*/false));
  const auto job = scheduler.GetJob(job_id);
  ASSERT_TRUE(job.has_value());
  EXPECT_EQ(job->state(), proto::Job::FAILED);
  EXPECT_NE(job->error().find("error: no matching function"),
            std::string::npos);

  const auto stored = store_->Get(candidate.candidate_id());
  ASSERT_TRUE(stored.has_value());
  EXPECT_EQ(stored->status(), proto::Candidate::BUILD_FAILED);
  EXPECT_NE(stored->build_error().find("no matching function"),
            std::string::npos);
  // Remaining batches are dropped: a build that failed cannot succeed on the
  // next opponent.
  EXPECT_EQ(scheduler.queued_batches(), 0);
}

// A host going away mid-match must return both slots and requeue the pair, or
// the surviving bot waits at the broker for a partner that will never build.
TEST_F(SchedulerTest, LosingAWorkerRequeuesTheWholePairedBatch) {
  const auto alpha = AddCandidate("Alpha");
  const auto beta = AddCandidate("Beta");
  auto first = std::make_shared<FakeWorker>("w1", 1);
  auto second = std::make_shared<FakeWorker>("w2", 1);
  scheduler_->AddWorker(first);
  scheduler_->AddWorker(second);

  std::string error;
  const auto job_id = scheduler_->EnqueueChallenge(
      alpha.candidate_id(), beta.candidate_id(), 2, &error);
  ASSERT_TRUE(job_id.has_value()) << error;
  ASSERT_EQ(scheduler_->in_flight_orders(), 2);

  scheduler_->RemoveWorker("w1");
  // The survivor was told to stop, so its slot comes back immediately instead
  // of after a rendezvous timeout.
  EXPECT_EQ(second->cancels.size(), 1u);
  EXPECT_EQ(scheduler_->in_flight_orders(), 0);
  EXPECT_EQ(scheduler_->worker_count(), 1);

  // Only one slot is left, so the requeued pair waits rather than half-running.
  EXPECT_EQ(scheduler_->queued_batches(), 1);
  auto replacement = std::make_shared<FakeWorker>("w3", 1);
  scheduler_->AddWorker(replacement);
  EXPECT_EQ(scheduler_->in_flight_orders(), 2);
}

TEST_F(SchedulerTest, RejectsUnusableChallenges) {
  const auto alpha = AddCandidate("Alpha");
  const auto pending = AddCandidate("Pending", proto::Candidate::PENDING);
  const auto broken = AddCandidate("Broken", proto::Candidate::BUILD_FAILED);
  std::string error;

  EXPECT_FALSE(scheduler_
                   ->EnqueueChallenge("no-such-id", "builtin:random", 2,
                                      &error)
                   .has_value());
  EXPECT_NE(error.find("unknown candidate"), std::string::npos);

  EXPECT_FALSE(scheduler_
                   ->EnqueueChallenge(alpha.candidate_id(),
                                      alpha.candidate_id(), 2, &error)
                   .has_value());
  EXPECT_NE(error.find("cannot play itself"), std::string::npos);

  EXPECT_FALSE(scheduler_
                   ->EnqueueChallenge(alpha.candidate_id(),
                                      pending.candidate_id(), 2, &error)
                   .has_value());
  EXPECT_NE(error.find("not ready"), std::string::npos);

  EXPECT_FALSE(scheduler_
                   ->EnqueueChallenge(broken.candidate_id(), "builtin:random",
                                      2, &error)
                   .has_value());
  EXPECT_NE(error.find("failed to build"), std::string::npos);

  EXPECT_FALSE(scheduler_
                   ->EnqueueChallenge(alpha.candidate_id(), "nonsense", 2,
                                      &error)
                   .has_value());
}

TEST_F(SchedulerTest, TopAndLadderResolveAgainstCurrentStandings) {
  const auto challenger = AddCandidate("Challenger");
  const auto weak = AddCandidate("Weak");
  const auto strong = AddCandidate("Strong");
  // Give "Strong" the higher rating by having it beat "Weak".
  elo_->RecordResult("risk2", strong.candidate_id(), weak.candidate_id(), 1.0);

  auto worker = std::make_shared<FakeWorker>("w1", 8);
  scheduler_->AddWorker(worker);

  std::string error;
  ASSERT_TRUE(
      scheduler_->EnqueueChallenge(challenger.candidate_id(), "top", 2, &error)
          .has_value())
      << error;
  ASSERT_EQ(worker->orders.size(), 2u);  // one pair
  EXPECT_EQ(worker->orders[0].opponent(), "player:" + strong.candidate_id());

  worker->orders.clear();
  ASSERT_TRUE(scheduler_
                  ->EnqueueChallenge(challenger.candidate_id(), "ladder", 2,
                                     &error)
                  .has_value())
      << error;
  // Two rivals available, ladder_size is 3, so it takes what exists: two pairs.
  EXPECT_EQ(worker->orders.size(), 4u);
}

TEST_F(SchedulerTest, GamesPerJobIsCapped) {
  const auto candidate = AddCandidate("Alpha");
  auto worker = std::make_shared<FakeWorker>("w1", 2);
  scheduler_->AddWorker(worker);

  std::string error;
  ASSERT_TRUE(scheduler_
                  ->EnqueueChallenge(candidate.candidate_id(),
                                     "builtin:random", 1000000, &error)
                  .has_value())
      << error;
  ASSERT_EQ(worker->orders.size(), 1u);
  EXPECT_EQ(worker->orders[0].num_games(), SchedulerConfig{}.max_games_per_job);
}

TEST_F(SchedulerTest, UnknownJobAndOrphanResultAreHandled) {
  EXPECT_FALSE(scheduler_->GetJob("nope").has_value());
  // A result for an order the scheduler never issued (a worker reconnecting
  // with stale state) must not crash or corrupt anything.
  scheduler_->OnResult("ghost", Result("o-unknown"));
  EXPECT_EQ(scheduler_->in_flight_orders(), 0);
}

}  // namespace
}  // namespace tournament_arena
