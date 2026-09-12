// The engine for the development loop: real processes, real timeouts, no
// daemon and no boundary.

#include "game_arena/sandbox/exec/process_engine.h"

#include <unistd.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

#include "gtest/gtest.h"

namespace sandbox_exec {
namespace {

auto Word(const std::string &text) -> proto::Token {
  proto::Token token;
  token.set_text(text);
  return token;
}

class ProcessEngineTest : public ::testing::Test {
 protected:
  void SetUp() override {
    root_ = std::filesystem::temp_directory_path() /
            ("process_engine_test_" + std::to_string(::getpid()));
    std::filesystem::remove_all(root_);
    std::filesystem::create_directories(root_ / "logs");
    std::filesystem::create_directories(root_ / "tree");
    std::filesystem::create_directories(root_ / "scratch");
  }
  void TearDown() override {
    std::error_code ec;
    std::filesystem::remove_all(root_, ec);
  }

  auto Script(const std::string &name, const std::string &body) -> std::string {
    const std::filesystem::path path = root_ / name;
    std::ofstream out(path);
    out << "#!/usr/bin/env bash\n" << body << "\n";
    out.close();
    std::filesystem::permissions(path, std::filesystem::perms::owner_all |
                                           std::filesystem::perms::group_exec |
                                           std::filesystem::perms::others_exec);
    return path.string();
  }

  auto BaseJob(const std::string &id) -> proto::Job {
    proto::Job job;
    job.set_id(id);
    job.set_log_dir((root_ / "logs").string());
    proto::Workspace *ws = job.mutable_workspace();
    ws->set_lower_dir((root_ / "tree").string());
    ws->set_upper_dir((root_ / "scratch").string());
    ws->set_overlay(proto::Workspace::OVERLAY_NONE);
    return job;
  }

  std::filesystem::path root_;
  ProcessEngine engine_;
};

TEST_F(ProcessEngineTest, SaysItIsNotABoundary) {
  // A caller whose job must be contained checks this rather than asking which
  // engine it was handed.
  EXPECT_FALSE(engine_.capabilities().isolates);
  EXPECT_EQ(engine_.name(), "process");
}

TEST_F(ProcessEngineTest, RunsAStepAndCapturesIt) {
  proto::Job job = BaseJob("j1");
  proto::Phase *phase = job.add_phases();
  phase->set_name("build");
  proto::Step *step = phase->mutable_foreground();
  step->set_name("build");
  step->set_timeout_s(30);
  *step->add_argv() = Word(Script("ok", "echo built; exit 0"));

  const proto::JobResult result = engine_.Run(job, nullptr);
  ASSERT_EQ(result.status().code(), proto::Status::OK)
      << result.status().message();
  ASSERT_EQ(result.phases(0).steps_size(), 1);
  EXPECT_EQ(result.phases(0).steps(0).exit_code(), 0);
  EXPECT_EQ(result.phases(0).steps(0).stdout(), "built\n");
}

TEST_F(ProcessEngineTest, ATimeoutReportsBothTheFactAndTheConvention) {
  proto::Job job = BaseJob("j2");
  proto::Phase *phase = job.add_phases();
  phase->set_name("slow");
  proto::Step *step = phase->mutable_foreground();
  step->set_name("slow");
  step->set_timeout_s(1);
  *step->add_argv() = Word(Script("slow", "sleep 30"));

  const proto::JobResult result = engine_.Run(job, nullptr);
  const proto::StepResult &step_result = result.phases(0).steps(0);
  EXPECT_TRUE(step_result.timed_out());
  // 124 as timeout(1) reports it, so a caller reading only the number is not
  // lied to about why this ended.
  EXPECT_EQ(step_result.exit_code(), 124);
  EXPECT_EQ(step_result.timeout_s(), 1);
}

TEST_F(ProcessEngineTest, AStepsEnvironmentReachesIt) {
  proto::Job job = BaseJob("j3");
  proto::Phase *phase = job.add_phases();
  phase->set_name("grade");
  proto::Step *step = phase->mutable_foreground();
  step->set_name("grade");
  step->set_timeout_s(30);
  (*step->mutable_env())["ARENA_REPORT"] =
      (root_ / "scratch" / "report.json").string();
  step->add_collect_files("report.json");
  *step->add_argv() = Word(
      Script("grade",
             "printf '{\"metrics\": {\"wall_ms\": 12}}' > \"$ARENA_REPORT\""));

  const proto::JobResult result = engine_.Run(job, nullptr);
  ASSERT_EQ(result.status().code(), proto::Status::OK)
      << result.status().message();
  // Collected off the scratch dir, without the engine knowing what the file
  // means.
  const auto &collected = result.phases(0).steps(0).collected();
  ASSERT_EQ(collected.count("report.json"), 1u);
  EXPECT_EQ(collected.at("report.json"), "{\"metrics\": {\"wall_ms\": 12}}");
}

TEST_F(ProcessEngineTest, ABackgroundStepsPortIsDiscoveredAndSubstituted) {
  proto::Job job = BaseJob("j4");
  proto::Phase *phase = job.add_phases();
  phase->set_name("match");

  proto::Step *peer = phase->add_background();
  peer->set_name("referee");
  peer->mutable_endpoint()->set_discover_via_port_file(true);
  peer->mutable_endpoint()->set_discover_timeout_s(10);
  *peer->add_argv() = Word(Script("referee", "echo 54321 > \"$1\"; sleep 0.3"));
  *peer->add_argv() = Word("{{port_file}}");

  proto::Step *bot = phase->mutable_foreground();
  bot->set_name("bot");
  bot->set_timeout_s(30);
  *bot->add_argv() = Word(Script("bot", "echo \"dialed $1\""));
  *bot->add_argv() = Word("{{peer:referee}}");

  const proto::JobResult result = engine_.Run(job, nullptr);
  ASSERT_EQ(result.status().code(), proto::Status::OK)
      << result.status().message();
  // The bot's argv carried the address the referee published, which is the
  // whole reason this engine cannot promise stable peer names: parallel jobs
  // on one host would collide on a fixed port.
  EXPECT_EQ(result.phases(0).steps(0).stdout(), "dialed localhost:54321\n");
  EXPECT_FALSE(engine_.capabilities().stable_peer_names);
}

TEST_F(ProcessEngineTest, APeerThatNeverListensFailsThePhase) {
  proto::Job job = BaseJob("j5");
  proto::Phase *phase = job.add_phases();
  phase->set_name("match");
  proto::Step *peer = phase->add_background();
  peer->set_name("referee");
  peer->mutable_endpoint()->set_discover_via_port_file(true);
  peer->mutable_endpoint()->set_discover_timeout_s(1);
  *peer->add_argv() = Word(Script("mute", "echo nothing 1>&2; sleep 3"));

  proto::Step *bot = phase->mutable_foreground();
  bot->set_name("bot");
  *bot->add_argv() = Word(Script("bot2", "true"));

  const proto::JobResult result = engine_.Run(job, nullptr);
  EXPECT_EQ(result.status().code(), proto::Status::ENDPOINT_FAILED);
  EXPECT_NE(result.status().message().find("never reported a port"),
            std::string::npos)
      << result.status().message();
}

TEST_F(ProcessEngineTest, CancelKillsEveryProcessGroupOfTheJob) {
  proto::Job job = BaseJob("j6");
  proto::Phase *phase = job.add_phases();
  phase->set_name("match");
  proto::Step *step = phase->mutable_foreground();
  step->set_name("bot");
  step->set_timeout_s(60);
  *step->add_argv() = Word(Script("blocker", "sleep 30"));

  std::thread canceller([this] {
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    engine_.Cancel("j6");
  });
  const auto started = std::chrono::steady_clock::now();
  const proto::JobResult result = engine_.Run(job, nullptr);
  const auto elapsed = std::chrono::steady_clock::now() - started;
  canceller.join();

  // Well short of the step's own 60s timeout.
  EXPECT_LT(elapsed, std::chrono::seconds(20));
  EXPECT_EQ(result.status().code(), proto::Status::CANCELLED);
}

TEST_F(ProcessEngineTest, CancelIsANoOpForAJobItIsNotRunning) {
  engine_.Cancel("never-heard-of-it");  // must not crash or block
}

}  // namespace
}  // namespace sandbox_exec
