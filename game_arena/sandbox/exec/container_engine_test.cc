// The engine against a fake docker, asserting the whole command line.
//
// The first test here is the hinge of the whole refactor: a job shaped like
// the one the fleet worker runs has to produce the *same* `docker run` argv
// that sandbox/worker's own golden test pins. If these two agree, moving the
// worker onto this engine cannot change what reaches the daemon.

#include "game_arena/sandbox/exec/container_engine.h"

#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "gtest/gtest.h"

namespace sandbox_exec {
namespace {

auto Word(const std::string &text, bool verbatim) -> proto::Token {
  proto::Token token;
  token.set_text(text);
  token.set_verbatim(verbatim);
  return token;
}

class ContainerEngineTest : public ::testing::Test {
 protected:
  void SetUp() override {
    root_ = std::filesystem::temp_directory_path() /
            ("container_engine_test_" + std::to_string(::getpid()));
    std::filesystem::remove_all(root_);
    std::filesystem::create_directories(root_ / "logs");
    std::filesystem::create_directories(root_ / "lower");

    std::ofstream docker(root_ / "docker");
    docker << "#!/usr/bin/env bash\n"
           << "echo \"docker $*\" >> \"" << (root_ / "docker.log").string()
           << "\"\n"
           << "case \"$1\" in\n"
           << "  run) echo RESULT-FROM-STEP;;\n"
           << "esac\n"
           << "exit 0\n";
    docker.close();
    std::filesystem::permissions(root_ / "docker",
                                 std::filesystem::perms::owner_all |
                                     std::filesystem::perms::group_exec |
                                     std::filesystem::perms::others_exec);

    std::ofstream mount(root_ / "mount");
    mount << "#!/usr/bin/env bash\nexit 0\n";
    mount.close();
    std::filesystem::permissions(root_ / "mount",
                                 std::filesystem::perms::owner_all |
                                     std::filesystem::perms::group_exec |
                                     std::filesystem::perms::others_exec);

    ContainerEngineConfig config;
    config.docker = (root_ / "docker").string();
    engine_ = std::make_unique<ContainerEngine>(config);
  }

  void TearDown() override {
    std::error_code ec;
    std::filesystem::remove_all(root_, ec);
  }

  auto Log() const -> std::string {
    std::ifstream in(root_ / "docker.log");
    return {std::istreambuf_iterator<char>(in),
            std::istreambuf_iterator<char>()};
  }

  // The argv of the `docker run` for |container|, up to the `-c` that
  // introduces the entrypoint script -- the same slice sandbox/worker's
  // golden test takes.
  auto RunArgvFor(const std::string &container) const -> std::string {
    const std::string log = Log();
    const std::string name_flag = "--name " + container + " ";
    const std::size_t at = log.find(name_flag);
    if (at == std::string::npos) {
      return "<no `docker run` for " + container + ">";
    }
    const std::size_t start = log.rfind("docker run", at);
    const std::size_t end = log.find(" -c ", at);
    return log.substr(start, end - start);
  }

  // A workspace laid out the way the fleet worker lays out a slot.
  auto SlotWorkspace() -> proto::Workspace {
    proto::Workspace ws;
    ws.set_lower_dir((root_ / "lower").string());
    ws.set_upper_dir((root_ / "overlay").string());
    ws.set_merged_dir((root_ / "merged").string());
    ws.set_staging_dir((root_ / "patches").string());
    ws.set_overlay(proto::Workspace::OVERLAY_HOST);
    ws.set_mount_binary((root_ / "mount").string());
    ws.set_umount_binary((root_ / "mount").string());
    ws.set_patch(proto::Workspace::PATCH_IN_ENTRYPOINT);
    ws.set_sandbox_work_dir("/workspace");

    proto::Mount *output_base = ws.add_mounts();
    output_base->set_source((root_ / "bazel_output_base").string());
    output_base->set_target("/output_base");
    proto::Mount *patches = ws.add_mounts();
    patches->set_source((root_ / "patches").string());
    patches->set_target("/patches");
    patches->set_readonly(true);
    proto::Mount *disk_cache = ws.add_mounts();
    disk_cache->set_source((root_ / "disk_cache").string());
    disk_cache->set_target("/disk_cache");
    return ws;
  }

  auto HardenedIsolation() -> proto::Isolation {
    proto::Isolation isolation;
    isolation.set_image("fake-image:1");
    isolation.set_memory_limit_mb(4096);
    isolation.set_pids_limit(512);
    proto::Tmpfs *tmpfs = isolation.add_tmpfs();
    tmpfs->set_target("/tmp");
    tmpfs->set_options("exec");
    return isolation;
  }

  std::filesystem::path root_;
  std::unique_ptr<ContainerEngine> engine_;
};

TEST_F(ContainerEngineTest, ABuildPhaseEmitsTheSameArgvTheWorkerDoesToday) {
  proto::Job job;
  job.set_id("saw-0-argv-1");
  job.set_log_dir((root_ / "logs").string());
  *job.mutable_workspace() = SlotWorkspace();
  *job.mutable_isolation() = HardenedIsolation();

  proto::Phase *phase = job.add_phases();
  phase->set_name("build");
  proto::Step *build = phase->mutable_foreground();
  build->set_name("build");
  build->set_applies_patches(true);
  build->set_timeout_s(1800);
  *build->add_argv() = Word("bazel", true);
  *build->add_argv() = Word("--output_base=/output_base", true);
  *build->add_argv() = Word("--disk_cache=/disk_cache", true);
  *build->add_argv() = Word("build", true);
  *build->add_argv() = Word("//solutions/c-ok:bot", false);

  const proto::JobResult result = engine_->Run(job, nullptr);
  ASSERT_EQ(result.status().code(), proto::Status::OK)
      << result.status().message();

  // Byte for byte what //game_arena/sandbox/worker's
  // WholeDockerRunArgvIsPinned expects, with this test's paths.
  EXPECT_EQ(RunArgvFor("saw-0-argv-1-build"),
            "docker run --rm --name saw-0-argv-1-build "
            "--cap-drop ALL --security-opt no-new-privileges --read-only "
            "--tmpfs /tmp:exec --memory 4096m --pids-limit 512 "
            "--network none "
            "--mount type=bind,source=" +
                (root_ / "merged").string() +
                ",target=/workspace "
                "--mount type=bind,source=" +
                (root_ / "overlay").string() +
                ",target=/sandbox "
                "--mount type=bind,source=" +
                (root_ / "bazel_output_base").string() +
                ",target=/output_base "
                "--mount type=bind,source=" +
                (root_ / "patches").string() +
                ",target=/patches,readonly "
                "--mount type=bind,source=" +
                (root_ / "disk_cache").string() +
                ",target=/disk_cache "
                "--entrypoint /bin/sh fake-image:1");
}

TEST_F(ContainerEngineTest, AMatchPhaseJoinsItsStepsOnAPrivateBridge) {
  proto::Job job;
  job.set_id("saw-0-ok-1");
  job.set_log_dir((root_ / "logs").string());
  *job.mutable_workspace() = SlotWorkspace();
  *job.mutable_isolation() = HardenedIsolation();

  proto::Phase *phase = job.add_phases();
  phase->set_name("match");
  proto::Isolation *bridged = phase->mutable_isolation();
  *bridged = HardenedIsolation();
  bridged->set_network(proto::Isolation::NETWORK_PHASE_BRIDGE);

  proto::Step *referee = phase->add_background();
  referee->set_name("referee");
  referee->set_keep_after_exit(true);
  *referee->add_argv() = Word("./bazel-bin/referee", false);

  proto::Step *bot = phase->mutable_foreground();
  bot->set_name("bot");
  bot->set_keep_after_exit(true);
  bot->set_timeout_s(60);
  *bot->add_argv() = Word("./bazel-bin/bot", false);

  const proto::JobResult result = engine_->Run(job, nullptr);
  ASSERT_EQ(result.status().code(), proto::Status::OK)
      << result.status().message();

  const std::string log = Log();
  // One bridge per job, not per phase: the name a Cancel derives. Created
  // before anything joins it, and removed after.
  EXPECT_NE(log.find("docker network create --internal saw-0-ok-1-net"),
            std::string::npos)
      << log;
  EXPECT_NE(log.find("--network saw-0-ok-1-net"), std::string::npos);
  EXPECT_NE(log.find("docker network rm saw-0-ok-1-net"),
            std::string::npos);
  // The referee runs detached and is kept, so its verdict can be asked for.
  EXPECT_NE(log.find("--name saw-0-ok-1-referee -d"), std::string::npos) << log;
  EXPECT_EQ(RunArgvFor("saw-0-ok-1-referee").find("--rm"), std::string::npos);
  // And it is drained rather than abandoned.
  EXPECT_NE(log.find("docker wait saw-0-ok-1-referee"), std::string::npos);
  EXPECT_NE(log.find("docker logs saw-0-ok-1-referee"), std::string::npos);

  // Both steps come back, background first.
  ASSERT_EQ(result.phases_size(), 1);
  ASSERT_EQ(result.phases(0).steps_size(), 2);
  EXPECT_EQ(result.phases(0).steps(0).name(), "bot");
  EXPECT_EQ(result.phases(0).steps(1).name(), "referee");
}

TEST_F(ContainerEngineTest, EveryContainerIsHardened) {
  proto::Job job;
  job.set_id("saw-0-hard-1");
  job.set_log_dir((root_ / "logs").string());
  *job.mutable_workspace() = SlotWorkspace();
  *job.mutable_isolation() = HardenedIsolation();

  proto::Phase *phase = job.add_phases();
  phase->set_name("match");
  proto::Step *peer = phase->add_background();
  peer->set_name("referee");
  *peer->add_argv() = Word("./referee", false);
  proto::Step *bot = phase->mutable_foreground();
  bot->set_name("bot");
  *bot->add_argv() = Word("./bot", false);

  ASSERT_EQ(engine_->Run(job, nullptr).status().code(), proto::Status::OK);

  // Walk every `docker run` line, as the worker's own test does: there is no
  // path through this engine that builds one without IsolationArgs.
  const std::string log = Log();
  std::size_t at = log.find("docker run");
  int seen = 0;
  while (at != std::string::npos) {
    const std::size_t end = log.find(" -c ", at);
    const std::string line = log.substr(at, end - at);
    EXPECT_NE(line.find("--cap-drop ALL"), std::string::npos) << line;
    EXPECT_NE(line.find("--security-opt no-new-privileges"), std::string::npos)
        << line;
    EXPECT_NE(line.find("--read-only"), std::string::npos) << line;
    EXPECT_EQ(line.find("SYS_ADMIN"), std::string::npos) << line;
    EXPECT_EQ(line.find("--network host"), std::string::npos) << line;
    ++seen;
    at = log.find("docker run", end);
  }
  EXPECT_EQ(seen, 2);
}

TEST_F(ContainerEngineTest, AFailedStepStopsTheJobAndLaterPhasesDoNotRun) {
  // A fake docker whose `run` fails.
  std::ofstream docker(root_ / "docker");
  docker << "#!/usr/bin/env bash\n"
         << "echo \"docker $*\" >> \"" << (root_ / "docker.log").string()
         << "\"\n"
         << "case \"$1\" in\n"
         << "  run) echo 'it broke' 1>&2; exit 7;;\n"
         << "esac\n"
         << "exit 0\n";
  docker.close();

  proto::Job job;
  job.set_id("saw-0-fail-1");
  job.set_log_dir((root_ / "logs").string());
  *job.mutable_workspace() = SlotWorkspace();
  *job.mutable_isolation() = HardenedIsolation();
  for (const char *name : {"build", "second"}) {
    proto::Phase *phase = job.add_phases();
    phase->set_name(name);
    proto::Step *step = phase->mutable_foreground();
    step->set_name(name);
    *step->add_argv() = Word("./x", false);
  }

  const proto::JobResult result = engine_->Run(job, nullptr);
  // A step that ran and failed is the job's business, not the engine's: the
  // status stays OK and the exit code carries the news.
  EXPECT_EQ(result.status().code(), proto::Status::OK);
  ASSERT_EQ(result.phases_size(), 1);
  EXPECT_EQ(result.phases(0).steps(0).exit_code(), 7);
  EXPECT_EQ(Log().find("--name saw-0-fail-1-second"), std::string::npos);
}

TEST_F(ContainerEngineTest, AMissingDockerIsTheEnginesFaultNotTheJobs) {
  ContainerEngineConfig config;
  config.docker = (root_ / "no-such-docker").string();
  ContainerEngine engine(config);

  proto::Job job;
  job.set_id("saw-0-nodocker-1");
  job.set_log_dir((root_ / "logs").string());
  *job.mutable_workspace() = SlotWorkspace();
  *job.mutable_isolation() = HardenedIsolation();
  proto::Phase *phase = job.add_phases();
  phase->set_name("build");
  proto::Step *step = phase->mutable_foreground();
  step->set_name("build");
  *step->add_argv() = Word("./x", false);

  const proto::JobResult result = engine.Run(job, nullptr);
  EXPECT_EQ(result.status().code(), proto::Status::TOOL_MISSING);
  EXPECT_NE(result.status().message().find("not found"), std::string::npos);
}

TEST_F(ContainerEngineTest, AJobNeedsAPhase) {
  proto::Job job;
  job.set_id("empty");
  job.set_log_dir((root_ / "logs").string());
  const proto::JobResult result = engine_->Run(job, nullptr);
  EXPECT_EQ(result.status().code(), proto::Status::INVALID_JOB);
}

TEST_F(ContainerEngineTest, CancelIsANoOpForAJobItIsNotRunning) {
  engine_->Cancel("never-heard-of-it");
  EXPECT_EQ(Log().find("kill"), std::string::npos);
}

}  // namespace
}  // namespace sandbox_exec
