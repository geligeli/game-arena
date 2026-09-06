// The local backend driving a graded order end to end.
//
// git and bazel are fakes, because neither is what this is testing. The graded
// command is real: a shell script that reads $ARENA_REPORT and writes numbers
// to it, exactly as a submitted benchmark would. That is the contract worth
// exercising -- the env var actually arriving, repeats actually repeating, the
// aggregate actually folding, and only the problem's metrics surviving.

#include <gtest/gtest.h>
#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

#include "game_mcts/tournament_server/sandbox/worker/local_backend.h"

namespace tournament_arena {
namespace {

constexpr char kFakeCommit[] = "c0ffee";

auto WriteScript(const std::filesystem::path &path,
                 const std::string &body) -> void {
  std::ofstream out(path);
  out << body;
  out.close();
  std::filesystem::permissions(path, std::filesystem::perms::owner_all);
}

class LocalGradeTest : public ::testing::Test {
 protected:
  void SetUp() override {
    root_ = std::filesystem::temp_directory_path() /
            ("local_grade_" + std::to_string(::getpid()) + "_" +
             std::to_string(::testing::UnitTest::GetInstance()->random_seed()));
    std::filesystem::remove_all(root_);
    std::filesystem::create_directories(root_);

    // git: a clone makes a .git dir, everything else succeeds. Applying a patch
    // is a no-op here; whether git can apply a diff is unified_diff_test's job.
    WriteScript(root_ / "git",
                "#!/usr/bin/env bash\n"
                "case \"$1\" in\n"
                "  clone) for a in \"$@\"; do dst=\"$a\"; done\n"
                "         mkdir -p \"$dst/.git\"; exit 0;;\n"
                "esac\n"
                "exit 0\n");
    WriteScript(root_ / "bazel", "#!/usr/bin/env bash\nexit 0\n");

    LocalBackendConfig config;
    config.repo_url = (root_ / "origin").string();
    config.work_dir = root_ / "work";
    config.git = (root_ / "git").string();
    config.bazel = (root_ / "bazel").string();
    std::filesystem::create_directories(root_ / "origin");
    backend_ = std::make_unique<LocalBackend>(config);
    std::string error;
    ASSERT_TRUE(backend_->Warmup(1, &error)) << error;
  }

  void TearDown() override { std::filesystem::remove_all(root_); }

  // A graded order whose command is |script|, run |repeats| times.
  auto MakeOrder(const std::string &script, int repeats,
                 proto::GradeOrder::Aggregate how) -> proto::WorkOrder {
    const auto path = root_ / "measure";
    WriteScript(path, script);

    proto::WorkOrder order;
    order.set_order_id("g-1");
    order.set_base_commit(kFakeCommit);
    order.mutable_candidate()->set_candidate_id("cand-1");
    order.mutable_candidate()->set_patch("diff --git a/x b/x\n");
    order.mutable_candidate()->add_build_targets("//bench");
    auto *grade = order.mutable_grade();
    grade->add_argv(path.string());
    grade->set_repeats(repeats);
    grade->set_aggregate(how);
    grade->add_metric_names("wall_ms");
    grade->set_timeout_s(60);
    return order;
  }

  std::filesystem::path root_;
  std::unique_ptr<LocalBackend> backend_;
};

// The command finds its report path in the environment and writes JSON there.
TEST_F(LocalGradeTest, ReadsTheJsonReportTheCommandWrites) {
  const OrderOutcome outcome = backend_->RunOrder(
      0, MakeOrder("#!/usr/bin/env bash\n"
                   "printf '{\"metrics\": {\"wall_ms\": 250.5}}' "
                   "> \"$ARENA_REPORT\"\n",
                   /*repeats=*/1, proto::GradeOrder::MIN));

  EXPECT_TRUE(outcome.build_ok);
  EXPECT_EQ(outcome.error, "");
  ASSERT_TRUE(outcome.metrics.contains("wall_ms"));
  EXPECT_DOUBLE_EQ(outcome.metrics.at("wall_ms"), 250.5);
  EXPECT_EQ(outcome.games_played, 1) << "runs, for a graded order";
}

// One timing is noise; the aggregate over the runs is the score.
TEST_F(LocalGradeTest, RepeatsTheRunAndTakesTheBest) {
  // Each run reports a different number, driven by a counter on disk.
  const OrderOutcome outcome = backend_->RunOrder(
      0,
      MakeOrder("#!/usr/bin/env bash\n"
                "n=$(cat /tmp/local_grade_counter 2>/dev/null || echo 0)\n"
                "n=$((n + 1)); echo $n > /tmp/local_grade_counter\n"
                "printf '{\"metrics\": {\"wall_ms\": %d}}' $((400 - n * 100))"
                " > \"$ARENA_REPORT\"\n",
                /*repeats=*/3, proto::GradeOrder::MIN));

  std::filesystem::remove("/tmp/local_grade_counter");
  EXPECT_EQ(outcome.error, "");
  ASSERT_TRUE(outcome.metrics.contains("wall_ms"));
  // 300, 200, 100 -> best of three.
  EXPECT_DOUBLE_EQ(outcome.metrics.at("wall_ms"), 100.0);
  EXPECT_EQ(outcome.games_played, 3);
}

// A benchmark that only knows how to print a line is not shut out.
TEST_F(LocalGradeTest, AcceptsAResultLineInsteadOfAReport) {
  const OrderOutcome outcome = backend_->RunOrder(
      0, MakeOrder("#!/usr/bin/env bash\necho 'RESULT wall_ms=42'\n", 1,
                   proto::GradeOrder::MIN));
  EXPECT_EQ(outcome.error, "");
  EXPECT_DOUBLE_EQ(outcome.metrics.at("wall_ms"), 42.0);
}

// A benchmark reporting more than the problem ranks on is normal; the extra is
// dropped rather than stored, so a report cannot grow the standings unbounded.
TEST_F(LocalGradeTest, KeepsOnlyTheProblemsMetrics) {
  const OrderOutcome outcome = backend_->RunOrder(
      0, MakeOrder("#!/usr/bin/env bash\n"
                   "printf '{\"metrics\": {\"wall_ms\": 10, \"noise\": 99}}' "
                   "> \"$ARENA_REPORT\"\n",
                   1, proto::GradeOrder::MIN));
  EXPECT_EQ(outcome.error, "");
  EXPECT_TRUE(outcome.metrics.contains("wall_ms"));
  EXPECT_FALSE(outcome.metrics.contains("noise"));
}

// A nonzero exit means the measurement is not trustworthy, whatever it printed.
TEST_F(LocalGradeTest, RefusesToScoreAFailedRun) {
  const OrderOutcome outcome = backend_->RunOrder(
      0,
      MakeOrder("#!/usr/bin/env bash\n"
                "printf '{\"metrics\": {\"wall_ms\": 1}}' > \"$ARENA_REPORT\"\n"
                "echo 'segfault' >&2\n"
                "exit 3\n",
                1, proto::GradeOrder::MIN));
  EXPECT_TRUE(outcome.metrics.empty()) << "a failed run must not be scored";
  EXPECT_NE(outcome.error.find("exited 3"), std::string::npos) << outcome.error;
  EXPECT_NE(outcome.error.find("segfault"), std::string::npos) << outcome.error;
}

TEST_F(LocalGradeTest, SaysSoWhenNothingWasMeasured) {
  const OrderOutcome outcome = backend_->RunOrder(
      0, MakeOrder("#!/usr/bin/env bash\necho 'ran, measured nothing'\n", 1,
                   proto::GradeOrder::MIN));
  EXPECT_TRUE(outcome.metrics.empty());
  EXPECT_NE(outcome.error.find("ARENA_REPORT"), std::string::npos)
      << outcome.error;
}

// The command reports a number the problem does not rank on, so there is
// nothing to place it by.
TEST_F(LocalGradeTest, SaysSoWhenTheProblemsMetricIsMissing) {
  const OrderOutcome outcome = backend_->RunOrder(
      0,
      MakeOrder("#!/usr/bin/env bash\n"
                "printf '{\"metrics\": {\"other\": 5}}' > \"$ARENA_REPORT\"\n",
                1, proto::GradeOrder::MIN));
  EXPECT_TRUE(outcome.metrics.empty());
  EXPECT_NE(outcome.error.find("none of this problem's metrics"),
            std::string::npos)
      << outcome.error;
}

// Cancelling work that is actually running, which is the case that matters.
// Dropping a queued order was always easy; this is the one that used to be
// documented as impossible ("an order already being built cannot be recalled").
TEST_F(LocalGradeTest, CancelStopsARunThatIsAlreadyUnderWay) {
  // A command that would run far longer than this test is willing to wait, and
  // leaves a marker so we can prove it was killed rather than left behind.
  const auto marker = root_ / "still_running";
  proto::WorkOrder order = MakeOrder(
      "#!/usr/bin/env bash\n"
      "touch \"" +
          marker.string() +
          "\"\n"
          "sleep 300\n",
      /*repeats=*/1, proto::GradeOrder::MIN);

  std::thread canceller([&] {
    // Wait until the command is demonstrably running, then pull the rug.
    for (int i = 0; i < 200 && !std::filesystem::exists(marker); ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    backend_->Cancel(order.order_id());
  });

  const auto started = std::chrono::steady_clock::now();
  const OrderOutcome outcome = backend_->RunOrder(0, order);
  const auto elapsed = std::chrono::steady_clock::now() - started;
  canceller.join();

  EXPECT_LT(elapsed, std::chrono::seconds(60))
      << "the cancel did not reach the running command";
  EXPECT_TRUE(std::filesystem::exists(marker)) << "the command never started";
  // Killed, so nothing was measured -- and a killed run must never be scored.
  EXPECT_TRUE(outcome.metrics.empty());
  EXPECT_FALSE(outcome.error.empty());
}

// A problem whose submissions can run arbitrary code at build time must not
// quietly land on a backend that runs them as the worker's own user.
TEST_F(LocalGradeTest, RefusesAnOrderThatRequiresAContainer) {
  proto::WorkOrder order = MakeOrder(
      "#!/usr/bin/env bash\nprintf '{\"metrics\": {\"wall_ms\": 1}}' "
      "> \"$ARENA_REPORT\"\n",
      1, proto::GradeOrder::MIN);
  order.set_require_container(true);

  const OrderOutcome outcome = backend_->RunOrder(0, order);

  EXPECT_FALSE(outcome.build_ok);
  EXPECT_TRUE(outcome.metrics.empty());
  EXPECT_NE(outcome.error.find("requires a container"), std::string::npos)
      << outcome.error;
}

// Cancelling something that is not running must be harmless: the stream thread
// does not know whether a slot has already finished.
TEST_F(LocalGradeTest, CancelIsANoOpForAnUnknownOrder) {
  backend_->Cancel("never-heard-of-it");
  const OrderOutcome outcome = backend_->RunOrder(
      0, MakeOrder(
             "#!/usr/bin/env bash\n"
             "printf '{\"metrics\": {\"wall_ms\": 7}}' > \"$ARENA_REPORT\"\n",
             1, proto::GradeOrder::MIN));
  EXPECT_EQ(outcome.error, "");
  EXPECT_DOUBLE_EQ(outcome.metrics.at("wall_ms"), 7.0);
}

}  // namespace
}  // namespace tournament_arena
