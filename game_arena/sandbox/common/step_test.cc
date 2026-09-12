// One step, its two log files, and the three ways it can fail to produce a
// result: the executable will not launch, it runs too long, or it exits
// nonzero. Every sandbox backend's error reporting is built on telling those
// apart, so they are worth pinning here rather than once per backend.

#include "game_arena/sandbox/common/step.h"

#include <unistd.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

#include "game_arena/sandbox/common/files.h"
#include "gtest/gtest.h"

namespace sandbox_common {
namespace {

using namespace std::chrono_literals;

class RunStepTest : public ::testing::Test {
 protected:
  void SetUp() override {
    root_ = std::filesystem::temp_directory_path() /
            ("step_test_" + std::to_string(::getpid()));
    std::filesystem::create_directories(root_);
  }
  void TearDown() override {
    std::error_code ec;
    std::filesystem::remove_all(root_, ec);
  }

  std::filesystem::path root_;
};

TEST_F(RunStepTest, SplitsTheTwoStreamsOnDiskAndJoinsThemInTheResult) {
  const StepResult step =
      RunStep("/bin/sh", {"-c", "echo to-stdout; echo to-stderr 1>&2"},
              /*cwd=*/{}, root_, "both", 30s);

  ASSERT_TRUE(step.run.started);
  EXPECT_EQ(step.run.exit_code, 0);
  EXPECT_FALSE(step.run.timed_out);

  // Apart on disk -- combining them into one file would interleave
  // unpredictably -- and concatenated only for reporting.
  EXPECT_EQ(ReadFile(root_ / "both.out"), "to-stdout\n");
  EXPECT_EQ(ReadFile(root_ / "both.err"), "to-stderr\n");
  EXPECT_EQ(step.output, "to-stdout\nto-stderr\n");
}

TEST_F(RunStepTest, ANonzeroExitIsStillAResult) {
  const StepResult step = RunStep("/bin/sh", {"-c", "echo why 1>&2; exit 3"},
                                  /*cwd=*/{}, root_, "failed", 30s);

  EXPECT_TRUE(step.run.started);
  EXPECT_EQ(step.run.exit_code, 3);
  EXPECT_FALSE(step.run.timed_out);
  EXPECT_EQ(step.output, "why\n");
}

TEST_F(RunStepTest, ATimeoutIsDistinctFromAFailure) {
  const StepResult step = RunStep("/bin/sh", {"-c", "sleep 30"},
                                  /*cwd=*/{}, root_, "slow", 1s);

  EXPECT_TRUE(step.run.started);
  EXPECT_TRUE(step.run.timed_out);
}

TEST_F(RunStepTest, AnExecutableThatWillNotLaunchIsNotStarted) {
  const StepResult step =
      RunStep((root_ / "nope").string(), {}, /*cwd=*/{}, root_, "missing", 30s);

  EXPECT_FALSE(step.run.started);
  EXPECT_FALSE(step.run.timed_out);
}

TEST_F(RunStepTest, RunsInTheGivenDirectory) {
  const std::filesystem::path where = root_ / "elsewhere";
  std::filesystem::create_directories(where);

  const StepResult step =
      RunStep("/bin/sh", {"-c", "pwd"}, where, root_, "cwd", 30s);

  ASSERT_TRUE(step.run.started);
  EXPECT_EQ(step.output, where.string() + "\n");
}

TEST_F(RunStepTest, PublishesTheProcessGroupSoSomeoneElseCanKillIt) {
  pid_t published = 0;
  const StepResult step =
      RunStep("/bin/sh", {"-c", "true"}, /*cwd=*/{}, root_, "pgid", 30s,
              /*address_space_limit_bytes=*/0,
              [&published](pid_t pgid) { published = pgid; });

  ASSERT_TRUE(step.run.started);
  // Its own group, not the test runner's: build tools spawn trees, and
  // killing only the parent leaves the workers running.
  EXPECT_GT(published, 0);
  EXPECT_NE(published, ::getpgrp());
}

TEST_F(RunStepTest, LogsLandUnderTheTagEvenWhenTheStepSaysNothing) {
  const StepResult step =
      RunStep("/bin/sh", {"-c", "true"}, /*cwd=*/{}, root_, "quiet", 30s);

  ASSERT_TRUE(step.run.started);
  EXPECT_TRUE(std::filesystem::exists(root_ / "quiet.out"));
  EXPECT_TRUE(std::filesystem::exists(root_ / "quiet.err"));
  EXPECT_EQ(step.output, "");
}

}  // namespace
}  // namespace sandbox_common
