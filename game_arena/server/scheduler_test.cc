// The scheduler, driven by fake workers so neither gRPC nor a real bazel build
// is in the loop.
//
// The behaviour that matters most here is batch atomicity: a
// candidate-vs-candidate match is two orders that must be dispatched together,
// because a lone half would sit at the broker's rendezvous until it timed out,
// holding a slot and producing no game.

#include "game_arena/server/scheduler.h"

#include <unistd.h>

#include <atomic>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "game_arena/server/elo_standings.h"
#include "gtest/gtest.h"

namespace tournament_arena {
namespace {

class FakeWorker : public FleetWorker {
 public:
  FakeWorker(std::string id, int slots) : id_(std::move(id)), slots_(slots) {}

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
            std::to_string(::testing::UnitTest::GetInstance()->random_seed()));
    std::filesystem::remove_all(dir_);
    SubmissionRules rules;
    rules.files_submit_dir = "solutions";
    rules.policy.add_allowed_dep_prefixes("//problem/lib:");
    rules.policy.add_allowed_dep_prefixes("//problem/lib:");
    rules.policy.add_allowed_dep_prefixes("//problem/game:");
    rules.policy.add_allowed_dep_prefixes("//problem/strategies:");
    rules.policy.add_allowed_dep_prefixes("@abseil-cpp//");
    rules.harness.set_api_dep("//problem/harness:api");
    rules.harness.set_main_src("//problem/harness:main.cc");
    rules.policy.add_allow_paths("solutions/**");
    store_ = std::make_unique<CandidateStore>(dir_ / "candidates",
                                              CandidateLimits{}, rules);
    elo_ = std::make_unique<tournament_broker::EloStore>(dir_ / "ratings.pb",
                                                         32.0);
    SchedulerConfig config;
    config.placement_opponents = {"builtin:random"};
    config.placement_games = 2;
    config.build_targets = {"//game_arena/candidates/{submission_id}:bot"};
    config.bot_target = "//game_arena/candidates/{submission_id}:bot";
    standings_ =
        std::make_unique<EloStandings>(elo_.get(), store_.get(), "risk2");
    scheduler_ = std::make_unique<Scheduler>(config, store_.get(), elo_.get(),
                                             standings_.get());
  }

  void TearDown() override { std::filesystem::remove_all(dir_); }

  // Tests run without a client registry, so metering is off and every
  // reservation is inert. The quota path has its own tests below.
  auto Reserve() -> Scheduler::Reservation {
    std::string error;
    auto reservation = scheduler_->TryReserve("", {}, false, &error);
    EXPECT_TRUE(reservation.has_value()) << error;
    return std::move(*reservation);
  }

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
  std::unique_ptr<EloStandings> standings_;
  std::unique_ptr<Scheduler> scheduler_;
};

TEST_F(SchedulerTest, PlacementDispatchesOneOrderPerBuiltin) {
  const auto candidate = AddCandidate("Alpha", proto::Candidate::PENDING);
  auto worker = std::make_shared<FakeWorker>("w1", 2);
  scheduler_->AddWorker(worker);

  const std::string job_id = scheduler_->EnqueuePlacement(candidate, Reserve());
  ASSERT_EQ(worker->orders.size(), 1u);
  const proto::WorkOrder &order = worker->orders[0];
  EXPECT_EQ(order.candidate().candidate_id(), candidate.candidate_id());
  EXPECT_EQ(order.opponent_spec(), "builtin:random");
  EXPECT_FALSE(order.has_opponent()) << "a builtin needs no second build";
  EXPECT_EQ(order.num_games(), 2);
  EXPECT_EQ(order.base_commit(), "commit0");
  // The patch rides along, so a worker needs nothing but the repo and the
  // order: no callback to the arena, no shared filesystem.
  EXPECT_NE(order.candidate().patch().find("+// Alpha"), std::string::npos)
      << order.candidate().patch();
  // Templates are expanded here; the worker never sees "{submission_id}".
  ASSERT_EQ(order.candidate().build_targets_size(), 1);
  EXPECT_EQ(order.candidate().build_targets(0),
            "//game_arena/candidates/" + candidate.candidate_id() + ":bot");
  EXPECT_EQ(order.candidate().bot_target(), order.candidate().build_targets(0));

  scheduler_->OnResult("w1", Result(order.order_id(), true, 2, 0));
  const auto job = scheduler_->GetJob(job_id);
  ASSERT_TRUE(job.has_value());
  EXPECT_EQ(job->state(), proto::Job::DONE);
  EXPECT_EQ(job->wins(), 2);
  EXPECT_EQ(job->games_played(), 2);
  EXPECT_EQ(store_->Get(candidate.candidate_id())->status(),
            proto::Candidate::READY);
}

TEST_F(SchedulerTest, EveryOrderCarriesTheProblemsSandboxAndTree) {
  // A worker has none of this: it takes one flag, the coordinator's address.
  // Two submissions are only comparable if they were built the same way, so
  // what to build and what the sandbox may do travel with every order -- the
  // argument base_commit already made for the tree's revision.
  SchedulerConfig config;
  config.placement_opponents = {"builtin:random"};
  config.placement_games = 2;
  config.repo_url = "/srv/arena";
  config.bazel_flags = {"--config=native"};
  config.turn_timeout_ms = 5000;
  config.game_time_budget_ms = 60000;
  config.max_moves_per_game = 100;
  config.sandbox.set_image("registry/arena-build:1");
  config.sandbox.set_memory_limit_mb(2048);
  config.sandbox.set_host_overlay(true);
  EloStandings standings(elo_.get(), store_.get(), "risk2");
  Scheduler scheduler(config, store_.get(), elo_.get(), &standings);

  const auto candidate = AddCandidate("Alpha", proto::Candidate::PENDING);
  auto worker = std::make_shared<FakeWorker>("w1", 2);
  scheduler.AddWorker(worker);
  scheduler.EnqueuePlacement(candidate, Reserve());

  ASSERT_FALSE(worker->orders.empty());
  const proto::WorkOrder &order = worker->orders[0];
  EXPECT_EQ(order.repo_url(), "/srv/arena");
  ASSERT_EQ(order.bazel_flags_size(), 1);
  EXPECT_EQ(order.bazel_flags(0), "--config=native");
  EXPECT_EQ(order.sandbox().image(), "registry/arena-build:1");
  EXPECT_EQ(order.sandbox().memory_limit_mb(), 2048u);
  EXPECT_TRUE(order.sandbox().host_overlay());
  // The referee's own limits, which used to reach it as its flag defaults
  // however the problem was configured.
  EXPECT_EQ(order.turn_timeout_ms(), 5000u);
  EXPECT_EQ(order.game_time_budget_ms(), 60000u);
  EXPECT_EQ(order.max_moves_per_game(), 100u);
}

TEST_F(SchedulerTest, ProgressSaysHowFarARunningJobHasGot) {
  const auto candidate = AddCandidate("Alpha", proto::Candidate::PENDING);
  auto worker = std::make_shared<FakeWorker>("w1", 2);
  scheduler_->AddWorker(worker);
  const std::string job_id = scheduler_->EnqueuePlacement(candidate, Reserve());
  ASSERT_EQ(worker->orders.size(), 1u);
  const std::string order_id = worker->orders[0].order_id();

  // Unset until a worker says something: a build can take half an hour, and
  // "running" on its own does not say whether to keep waiting.
  EXPECT_EQ(scheduler_->GetJob(job_id)->phase(), proto::OrderProgress::CLONING);

  proto::OrderProgress progress;
  progress.set_order_id(order_id);
  progress.set_phase(proto::OrderProgress::BUILDING);
  scheduler_->OnProgress(progress);
  EXPECT_EQ(scheduler_->GetJob(job_id)->phase(),
            proto::OrderProgress::BUILDING);

  progress.set_phase(proto::OrderProgress::RUNNING);
  scheduler_->OnProgress(progress);
  EXPECT_EQ(scheduler_->GetJob(job_id)->phase(), proto::OrderProgress::RUNNING);

  scheduler_->OnResult("w1", Result(order_id, true, 2, 0));
  EXPECT_EQ(scheduler_->GetJob(job_id)->state(), proto::Job::DONE);
}

TEST_F(SchedulerTest, ProgressForARetiredOrderIsIgnored) {
  const auto candidate = AddCandidate("Alpha", proto::Candidate::PENDING);
  auto worker = std::make_shared<FakeWorker>("w1", 2);
  scheduler_->AddWorker(worker);
  const std::string job_id = scheduler_->EnqueuePlacement(candidate, Reserve());
  const std::string order_id = worker->orders[0].order_id();
  scheduler_->OnResult("w1", Result(order_id, true, 2, 0));

  // A progress report racing the result that retired its order is normal, not
  // an error, and must not resurrect or disturb a finished job.
  proto::OrderProgress progress;
  progress.set_order_id(order_id);
  progress.set_phase(proto::OrderProgress::BUILDING);
  scheduler_->OnProgress(progress);

  const auto job = scheduler_->GetJob(job_id);
  ASSERT_TRUE(job.has_value());
  EXPECT_EQ(job->state(), proto::Job::DONE);
}

TEST_F(SchedulerTest, CandidateMatchDispatchesBothSidesNamingEachOther) {
  const auto alpha = AddCandidate("Alpha");
  const auto beta = AddCandidate("Beta");
  auto worker = std::make_shared<FakeWorker>("w1", 4);
  scheduler_->AddWorker(worker);

  std::string error;
  const auto job_id = scheduler_->EnqueueChallenge(
      alpha.candidate_id(), beta.candidate_id(), 6, Reserve(), &error);
  ASSERT_TRUE(job_id.has_value()) << error;

  // One order, both sides. It used to be two mirrored orders that had to be
  // dispatched together; the referee moving into the sandbox removed the pair.
  ASSERT_EQ(worker->orders.size(), 1u);
  const auto &order = worker->orders[0];
  EXPECT_EQ(order.candidate().candidate_id(), alpha.candidate_id());
  EXPECT_EQ(order.opponent_spec(), "player:" + beta.candidate_id());
  EXPECT_EQ(order.num_games(), 6);

  // The opponent's own patch travels in the same order, so one worker can
  // build both sides and referee them without another worker's cooperation.
  ASSERT_TRUE(order.has_opponent());
  EXPECT_EQ(order.opponent().candidate_id(), beta.candidate_id());
  EXPECT_FALSE(order.opponent().patch().empty());
  EXPECT_EQ(order.opponent().bot_target(),
            "//game_arena/candidates/" + beta.candidate_id() + ":bot");
  // Disjoint paths, which is what lets both patches apply to one checkout.
  EXPECT_NE(order.candidate().patch(), order.opponent().patch());

  scheduler_->OnResult("w1", Result(order.order_id(), true, 4, 2));

  const auto job = scheduler_->GetJob(*job_id);
  ASSERT_TRUE(job.has_value());
  EXPECT_EQ(job->state(), proto::Job::DONE);
  EXPECT_EQ(job->wins(), 4);
  EXPECT_EQ(job->losses(), 2);
  EXPECT_EQ(job->games_played(), 6);
}

// The core invariant: never dispatch half a pair.
// The inverse of the rule this replaced: a candidate-vs-candidate match used
// to need two free slots at once, because half a pair was a bot parked at a
// rendezvous with no partner. One order is now one whole match, so one slot
// runs it.
TEST_F(SchedulerTest, AMatchNeedsOnlyOneSlot) {
  const auto alpha = AddCandidate("Alpha");
  const auto beta = AddCandidate("Beta");
  auto small = std::make_shared<FakeWorker>("w1", 1);
  scheduler_->AddWorker(small);

  std::string error;
  const auto job_id = scheduler_->EnqueueChallenge(
      alpha.candidate_id(), beta.candidate_id(), 2, Reserve(), &error);
  ASSERT_TRUE(job_id.has_value()) << error;

  ASSERT_EQ(small->orders.size(), 1u);
  EXPECT_TRUE(small->orders[0].has_opponent());
  EXPECT_EQ(scheduler_->in_flight_orders(), 1);
  EXPECT_EQ(scheduler_->queued_orders(), 0);
}

TEST_F(SchedulerTest, BatchesQueueUntilAWorkerAttaches) {
  const auto candidate = AddCandidate("Alpha", proto::Candidate::PENDING);
  const std::string job_id = scheduler_->EnqueuePlacement(candidate, Reserve());
  EXPECT_EQ(scheduler_->queued_orders(), 1);
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
  EloStandings standings(elo_.get(), store_.get(), "risk2");
  Scheduler scheduler(config, store_.get(), elo_.get(), &standings);
  scheduler.AddWorker(worker);
  const std::string job_id = scheduler.EnqueuePlacement(candidate, Reserve());
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
  EXPECT_EQ(scheduler.queued_orders(), 0);
}

// A host going away mid-match must return both slots and requeue the pair, or
// the surviving bot waits at the broker for a partner that will never build.
TEST_F(SchedulerTest, LosingAWorkerRequeuesItsOrder) {
  const auto alpha = AddCandidate("Alpha");
  const auto beta = AddCandidate("Beta");
  auto first = std::make_shared<FakeWorker>("w1", 1);
  scheduler_->AddWorker(first);

  std::string error;
  const auto job_id = scheduler_->EnqueueChallenge(
      alpha.candidate_id(), beta.candidate_id(), 2, Reserve(), &error);
  ASSERT_TRUE(job_id.has_value()) << error;
  ASSERT_EQ(scheduler_->in_flight_orders(), 1);

  scheduler_->RemoveWorker("w1");
  EXPECT_EQ(scheduler_->in_flight_orders(), 0);
  EXPECT_EQ(scheduler_->worker_count(), 0);
  // Requeued whole. There is no sibling order on another host to cancel, which
  // is the bookkeeping the collapse removed.
  EXPECT_EQ(scheduler_->queued_orders(), 1);

  auto replacement = std::make_shared<FakeWorker>("w2", 1);
  scheduler_->AddWorker(replacement);
  EXPECT_EQ(scheduler_->in_flight_orders(), 1);
  ASSERT_EQ(replacement->orders.size(), 1u);
  EXPECT_EQ(replacement->orders[0].candidate().candidate_id(),
            alpha.candidate_id());
}

TEST_F(SchedulerTest, RejectsUnusableChallenges) {
  const auto alpha = AddCandidate("Alpha");
  const auto pending = AddCandidate("Pending", proto::Candidate::PENDING);
  const auto broken = AddCandidate("Broken", proto::Candidate::BUILD_FAILED);
  std::string error;

  EXPECT_FALSE(scheduler_
                   ->EnqueueChallenge("no-such-id", "builtin:random", 2,
                                      Reserve(), &error)
                   .has_value());
  EXPECT_NE(error.find("unknown candidate"), std::string::npos);

  EXPECT_FALSE(scheduler_
                   ->EnqueueChallenge(alpha.candidate_id(),
                                      alpha.candidate_id(), 2, Reserve(),
                                      &error)
                   .has_value());
  EXPECT_NE(error.find("cannot play itself"), std::string::npos);

  EXPECT_FALSE(scheduler_
                   ->EnqueueChallenge(alpha.candidate_id(),
                                      pending.candidate_id(), 2, Reserve(),
                                      &error)
                   .has_value());
  EXPECT_NE(error.find("not ready"), std::string::npos);

  EXPECT_FALSE(scheduler_
                   ->EnqueueChallenge(broken.candidate_id(), "builtin:random",
                                      2, Reserve(), &error)
                   .has_value());
  EXPECT_NE(error.find("failed to build"), std::string::npos);

  EXPECT_FALSE(scheduler_
                   ->EnqueueChallenge(alpha.candidate_id(), "nonsense", 2,
                                      Reserve(), &error)
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
  ASSERT_TRUE(scheduler_
                  ->EnqueueChallenge(challenger.candidate_id(), "top", 2,
                                     Reserve(), &error)
                  .has_value())
      << error;
  ASSERT_EQ(worker->orders.size(), 1u);  // one order is one whole match
  EXPECT_EQ(worker->orders[0].opponent_spec(),
            "player:" + strong.candidate_id());

  worker->orders.clear();
  ASSERT_TRUE(scheduler_
                  ->EnqueueChallenge(challenger.candidate_id(), "ladder", 2,
                                     Reserve(), &error)
                  .has_value())
      << error;
  // Two rivals available and ladder_size is 3, so it takes what exists: one
  // order per rival.
  EXPECT_EQ(worker->orders.size(), 2u);
}

TEST_F(SchedulerTest, GamesPerJobIsCapped) {
  const auto candidate = AddCandidate("Alpha");
  auto worker = std::make_shared<FakeWorker>("w1", 2);
  scheduler_->AddWorker(worker);

  std::string error;
  ASSERT_TRUE(scheduler_
                  ->EnqueueChallenge(candidate.candidate_id(), "builtin:random",
                                     1000000, Reserve(), &error)
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

// --- quotas ---------------------------------------------------------------
//
// The reservation exists so a check and its claim are one locked step. Two
// concurrent submits from one client must not both pass, and a
// check-then-enqueue would let them.

class QuotaTest : public SchedulerTest {
 protected:
  auto Quota(int active, int queued) -> proto::ClientQuota {
    proto::ClientQuota quota;
    quota.set_max_active_evaluations(active);
    quota.set_max_queued_jobs(queued);
    return quota;
  }
};

TEST_F(QuotaTest, RefusesASecondEvaluationWhileOneIsRunning) {
  const auto alpha = AddCandidate("Alpha", proto::Candidate::PENDING);
  auto worker = std::make_shared<FakeWorker>("w1", 4);
  scheduler_->AddWorker(worker);

  std::string error;
  auto first = scheduler_->TryReserve("agent-1", Quota(1, 8), false, &error);
  ASSERT_TRUE(first.has_value()) << error;
  const std::string job_id =
      scheduler_->EnqueuePlacement(alpha, std::move(*first));
  ASSERT_EQ(scheduler_->GetJob(job_id)->state(), proto::Job::RUNNING);

  // The client's one slot is spent.
  auto second = scheduler_->TryReserve("agent-1", Quota(1, 8), false, &error);
  EXPECT_FALSE(second.has_value());
  EXPECT_NE(error.find("already have"), std::string::npos) << error;
  EXPECT_NE(error.find(job_id), std::string::npos)
      << "the error should name what to wait for: " << error;

  // Another client is unaffected: the quota is per client, not global.
  EXPECT_TRUE(
      scheduler_->TryReserve("agent-2", Quota(1, 8), false, &error).has_value())
      << error;

  // Once it finishes, the slot comes back.
  scheduler_->OnResult("w1", Result(worker->orders[0].order_id(), true, 2, 0));
  EXPECT_TRUE(
      scheduler_->TryReserve("agent-1", Quota(1, 8), false, &error).has_value())
      << error;
}

// The specific bug the reservation exists to prevent. A check-then-enqueue
// would let both of these through.
TEST_F(QuotaTest, ConcurrentReservesGrantExactlyOne) {
  constexpr int kThreads = 8;
  std::atomic<int> granted{0};
  std::vector<std::thread> threads;
  std::vector<std::optional<Scheduler::Reservation>> held(kThreads);

  for (int i = 0; i < kThreads; ++i) {
    threads.emplace_back([&, i] {
      std::string error;
      held[i] = scheduler_->TryReserve("agent-1", Quota(1, 1), false, &error);
      if (held[i].has_value()) {
        ++granted;
      }
    });
  }
  for (std::thread &thread : threads) {
    thread.join();
  }
  EXPECT_EQ(granted.load(), 1)
      << "two submits from one client both passed the quota";
}

// A refused submission must leave nothing behind, and an unused reservation
// must not hold the slot until restart.
TEST_F(QuotaTest, AnUnusedReservationIsReleased) {
  std::string error;
  {
    auto reservation =
        scheduler_->TryReserve("agent-1", Quota(1, 1), false, &error);
    ASSERT_TRUE(reservation.has_value()) << error;
    EXPECT_FALSE(scheduler_->TryReserve("agent-1", Quota(1, 1), false, &error)
                     .has_value())
        << "the held slot should still count";
  }  // dropped without being enqueued -- the submission was rejected
  EXPECT_TRUE(
      scheduler_->TryReserve("agent-1", Quota(1, 1), false, &error).has_value())
      << error;
}

TEST_F(QuotaTest, BoundsQueuedJobsSeparatelyFromRunningOnes) {
  const auto alpha = AddCandidate("Alpha", proto::Candidate::PENDING);
  std::string error;
  // No workers attached, so everything queues rather than running.
  auto first = scheduler_->TryReserve("agent-1", Quota(1, 2), false, &error);
  ASSERT_TRUE(first.has_value()) << error;
  scheduler_->EnqueuePlacement(alpha, std::move(*first));

  auto second = scheduler_->TryReserve("agent-1", Quota(1, 2), false, &error);
  ASSERT_TRUE(second.has_value()) << error;
  scheduler_->EnqueuePlacement(alpha, std::move(*second));

  EXPECT_FALSE(scheduler_->TryReserve("agent-1", Quota(1, 2), false, &error)
                   .has_value());
  EXPECT_NE(error.find("queued"), std::string::npos) << error;
}

TEST_F(QuotaTest, CancelRunningReplacesTheClientsInFlightWork) {
  const auto alpha = AddCandidate("Alpha", proto::Candidate::PENDING);
  auto worker = std::make_shared<FakeWorker>("w1", 4);
  scheduler_->AddWorker(worker);

  std::string error;
  auto first = scheduler_->TryReserve("agent-1", Quota(1, 8), false, &error);
  ASSERT_TRUE(first.has_value()) << error;
  const std::string old_job =
      scheduler_->EnqueuePlacement(alpha, std::move(*first));
  ASSERT_EQ(worker->orders.size(), 1u);

  auto replacement = scheduler_->TryReserve("agent-1", Quota(1, 8),
                                            /*cancel_running=*/true, &error);
  ASSERT_TRUE(replacement.has_value()) << error;
  EXPECT_EQ(replacement->superseded(), std::vector<std::string>{old_job});

  // CANCELLED, not FAILED: an agent needs to tell "you replaced it" from "it
  // broke", and only one of those is worth investigating.
  EXPECT_EQ(scheduler_->GetJob(old_job)->state(), proto::Job::CANCELLED);
  EXPECT_NE(scheduler_->GetJob(old_job)->error().find("superseded"),
            std::string::npos);
  // The worker was told to stop, so the slot comes back now.
  EXPECT_EQ(worker->cancels.size(), 1u);
  EXPECT_EQ(scheduler_->in_flight_orders(), 0);

  scheduler_->EnqueuePlacement(alpha, std::move(*replacement));
  EXPECT_EQ(worker->orders.size(), 2u);
}

// Metering is off when no registry is configured, and the reservation is inert.
TEST_F(QuotaTest, NoClientIdMeansNoLimit) {
  std::string error;
  for (int i = 0; i < 5; ++i) {
    EXPECT_TRUE(scheduler_->TryReserve("", {}, false, &error).has_value())
        << error;
  }
}

}  // namespace
}  // namespace tournament_arena
