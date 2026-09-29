// The engine against a real docker daemon: what the fake-docker tests pin as
// argv, checked here as behaviour. Manual and local, since it needs this
// host's daemon and an image with sh and git, the arena's base by default:
//
//   bazel test //game_arena/sandbox/exec:container_engine_docker_test
//   ARENA_IT_IMAGE=<image> bazel test --test_env=ARENA_IT_IMAGE ...

#include <unistd.h>

#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>

#include "game_arena/sandbox/exec/container_engine.h"
#include "gtest/gtest.h"

namespace sandbox_exec {
namespace {

std::string Image() {
  const char *image = std::getenv("ARENA_IT_IMAGE");
  return image != nullptr
             ? image
             : "registry.takumi.city/game-arena-base:noble-bazel8.8.0-nano";
}

// Stdout of a docker command, for setting up and inspecting the daemon.
std::string Docker(const std::string &args) {
  std::string out;
  FILE *pipe = ::popen(("docker " + args + " 2>/dev/null").c_str(), "r");
  std::array<char, 4096> buffer;
  while (std::fgets(buffer.data(), buffer.size(), pipe) != nullptr) {
    out += buffer.data();
  }
  ::pclose(pipe);
  return out;
}

proto::Token Word(const std::string &text, bool verbatim = true) {
  proto::Token token;
  token.set_text(text);
  token.set_verbatim(verbatim);
  return token;
}

class ContainerEngineDockerTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_NE(Docker("image inspect -f ok " + Image()), "")
        << "no image " << Image() << " on this daemon";
    root_ = std::filesystem::temp_directory_path() /
            ("container_engine_docker_" + std::to_string(::getpid()));
    std::filesystem::create_directories(root_ / "logs");
    id_ = "saw-it-" + std::to_string(::getpid()) + "-" +
          ::testing::UnitTest::GetInstance()->current_test_info()->name();
    for (char &c : id_) {
      c = std::tolower(static_cast<unsigned char>(c));
    }
  }

  void TearDown() override {
    if (HasFailure()) {
      std::cerr << "the engine's logs are kept in " << root_ / "logs" << "\n";
      return;
    }
    std::error_code ec;
    std::filesystem::remove_all(root_, ec);
  }

  // One phase whose step applies a patch that creates hello.txt, then runs
  // |script| in the patched workspace.
  proto::Job Job(const std::string &script) {
    proto::Job job;
    job.set_id(id_);
    job.set_log_dir((root_ / "logs").string());
    proto::Workspace *ws = job.mutable_workspace();
    ws->set_staging_dir((root_ / "patches").string());
    proto::StagedFile *patch = ws->add_staged_files();
    patch->set_path("c.diff");
    patch->set_content(
        "diff --git a/hello.txt b/hello.txt\n"
        "new file mode 100644\n"
        "--- /dev/null\n"
        "+++ b/hello.txt\n"
        "@@ -0,0 +1 @@\n"
        "+hello from the patch\n");
    ws->add_patch_files("c.diff");
    ws->set_patch(proto::Workspace::PATCH_IN_ENTRYPOINT);
    ws->set_sandbox_work_dir("/workspace");

    proto::Isolation *isolation = job.mutable_isolation();
    isolation->set_image(Image());
    proto::Tmpfs *tmpfs = isolation->add_tmpfs();
    tmpfs->set_target("/tmp");
    tmpfs->set_options("exec");

    proto::Phase *phase = job.add_phases();
    phase->set_name("build");
    // Whole, as the worker writes it: a phase's isolation replaces the job's.
    *phase->mutable_isolation() = job.isolation();
    proto::Step *step = phase->mutable_foreground();
    step->set_name("build");
    step->set_applies_patches(true);
    step->set_timeout_s(120);
    for (const char *word : {"sh", "-c"}) {
      *step->add_argv() = Word(word);
    }
    // Quoted: a verbatim token is pasted into the entrypoint script as is.
    *step->add_argv() = Word(script, /*verbatim=*/false);
    return job;
  }

  static const proto::StepResult &Step(const proto::JobResult &result) {
    return result.phases(0).steps(0);
  }

  // Everything the daemon still has under this job's names.
  std::string Leftovers() const {
    return Docker("ps -aq --filter name=" + id_) +
           Docker("volume ls -q --filter name=" + id_) +
           Docker("network ls -q --filter name=" + id_);
  }

  std::filesystem::path root_;
  std::string id_;
  ContainerEngine engine_{ContainerEngineConfig{}};
};

TEST_F(ContainerEngineDockerTest, TheCapsAreWhatTheCgroupEnforces) {
  proto::Job job =
      Job("cat hello.txt /sys/fs/cgroup/memory.max "
          "/sys/fs/cgroup/cpu.max /sys/fs/cgroup/pids.max");
  job.mutable_phases(0)->mutable_isolation()->set_memory_limit_mb(512);
  job.mutable_phases(0)->mutable_isolation()->set_cpus(1);
  job.mutable_phases(0)->mutable_isolation()->set_pids_limit(64);

  const proto::JobResult result = engine_.Run(job, nullptr);
  ASSERT_EQ(result.status().code(), proto::Status::OK)
      << result.status().message();
  const std::string out = Step(result).stdout();
  EXPECT_NE(out.find("hello from the patch"), std::string::npos)
      << Step(result).DebugString();
  EXPECT_NE(out.find(std::to_string(512LL << 20)), std::string::npos) << out;
  EXPECT_NE(out.find("100000 100000"), std::string::npos) << out;
  EXPECT_NE(out.find("\n64"), std::string::npos) << out;
}

TEST_F(ContainerEngineDockerTest, ABuildWithNoCapHasNone) {
  const proto::JobResult result =
      engine_.Run(Job("cat /sys/fs/cgroup/memory.max"), nullptr);
  ASSERT_EQ(result.status().code(), proto::Status::OK);
  EXPECT_NE(Step(result).stdout().find("max"), std::string::npos)
      << Step(result).stdout();
}

// What a match runs: a binary from the archive, readable and runnable by the
// sandbox's user and writable by nobody.
TEST_F(ContainerEngineDockerTest, InputsRunAndCannotBeChanged) {
  const std::filesystem::path tool = root_ / "tool";
  std::ofstream(tool) << "#!/bin/sh\necho ran from the archive\n";
  std::filesystem::permissions(tool, std::filesystem::perms::owner_all |
                                         std::filesystem::perms::group_read |
                                         std::filesystem::perms::group_exec |
                                         std::filesystem::perms::others_read |
                                         std::filesystem::perms::others_exec);
  proto::Job job = Job("/inputs/tool && ! touch /inputs/tool 2>/dev/null");
  proto::InputFile *input = job.mutable_workspace()->add_inputs();
  input->set_source(tool.string());
  input->set_name("tool");
  job.mutable_workspace()->set_inputs_mount("/inputs");
  job.mutable_isolation()->set_run_as_user("1000:1000");
  *job.mutable_phases(0)->mutable_isolation() = job.isolation();

  const proto::JobResult result = engine_.Run(job, nullptr);
  ASSERT_EQ(result.status().code(), proto::Status::OK) << result.DebugString();
  EXPECT_EQ(Step(result).exit_code(), 0) << Step(result).DebugString();
  EXPECT_EQ(Step(result).stdout(), "ran from the archive\n");
  EXPECT_EQ(Leftovers(), "");
}

// A worker killed mid-job leaves its containers, and they hold the job's
// volumes. The order comes back under the same job id -- on a daemon shared
// by several workers, possibly to another one -- and must start clean
// rather than meet its own patch ("already exists in working directory").
TEST_F(ContainerEngineDockerTest, ARedeliveredJobStartsClean) {
  const proto::Job job = Job("cat hello.txt");
  const std::string ws = SandboxName(id_, "ws");
  Docker("volume create " + ws);
  Docker("run --name " + SandboxName(id_, "build") + " -v " + ws +
         ":/workspace --entrypoint sh " + Image() +
         " -c 'echo stale > /workspace/hello.txt'");
  Docker("create --name " + SandboxName(id_, "load") + " -v " + ws +
         ":/workspace " + Image());

  const proto::JobResult result = engine_.Run(job, nullptr);
  ASSERT_EQ(result.status().code(), proto::Status::OK)
      << result.status().message();
  EXPECT_EQ(Step(result).exit_code(), 0) << Step(result).stderr();
  EXPECT_EQ(Step(result).stdout(), "hello from the patch\n");
  EXPECT_EQ(Leftovers(), "");
}

// Two steps on the phase's own network: the job ends with no container,
// volume or network left, or 64 slots exhaust the daemon's address pools.
TEST_F(ContainerEngineDockerTest, ABridgedJobLeavesNothingBehind) {
  proto::Job job = Job("true");
  proto::Phase *phase = job.mutable_phases(0);
  phase->set_name("match");
  phase->mutable_isolation()->set_network(
      proto::Isolation::NETWORK_PHASE_BRIDGE);
  proto::Step *server = phase->add_background();
  server->set_name("server");
  *server->add_argv() = Word("sh");
  *server->add_argv() = Word("-c");
  *server->add_argv() = Word("sleep 30", /*verbatim=*/false);
  phase->mutable_foreground()->mutable_argv(2)->set_text(
      "getent hosts " + SandboxName(id_, "server"));

  const proto::JobResult result = engine_.Run(job, nullptr);
  ASSERT_EQ(result.status().code(), proto::Status::OK) << result.DebugString();
  EXPECT_EQ(Step(result).exit_code(), 0)
      << "the steps did not share a network: " << Step(result).stderr();
  EXPECT_EQ(Leftovers(), "");
}

TEST_F(ContainerEngineDockerTest, ACancelledJobLeavesNothingBehind) {
  const proto::Job job = Job("sleep 60");
  std::thread cancel([&] {
    for (int i = 0; i < 100 && Docker("ps -q --filter name=" +
                                      SandboxName(id_, "build")) == "";
         ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    engine_.Cancel(id_);
  });
  const proto::JobResult result = engine_.Run(job, nullptr);
  cancel.join();
  EXPECT_EQ(result.status().code(), proto::Status::CANCELLED)
      << result.status().message();
  EXPECT_EQ(Leftovers(), "");
}

}  // namespace
}  // namespace sandbox_exec
