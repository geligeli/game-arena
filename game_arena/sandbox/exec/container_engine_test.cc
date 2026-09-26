// The engine against a fake docker, asserting the whole command line.
//
// The first test here is the hinge: a job shaped like the one the fleet
// worker runs has to produce the *same* `docker run` argv that
// sandbox/worker's own golden test pins. If these two agree, moving the
// worker onto this engine cannot change what reaches the daemon.

#include "game_arena/sandbox/exec/container_engine.h"

#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "gtest/gtest.h"

namespace sandbox_exec {
namespace {

proto::Token Word(const std::string &text, bool verbatim) {
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

    ContainerEngineConfig config;
    config.docker = (root_ / "docker").string();
    engine_ = std::make_unique<ContainerEngine>(config);
  }

  void TearDown() override {
    std::error_code ec;
    std::filesystem::remove_all(root_, ec);
  }

  std::string Log() const {
    std::ifstream in(root_ / "docker.log");
    return {std::istreambuf_iterator<char>(in),
            std::istreambuf_iterator<char>()};
  }

  // The argv of the `docker run` for |container|, up to the `-c` that
  // introduces the entrypoint script -- the same slice sandbox/worker's
  // golden test takes.
  std::string RunArgvFor(const std::string &container) const {
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

  // A workspace laid out the way the fleet worker lays out a slot: the tree
  // and the staged patch on this side, the persistent output base a volume.
  proto::Workspace SlotWorkspace() {
    proto::Workspace ws;
    ws.set_tree_dir((root_ / "lower").string());
    ws.set_staging_dir((root_ / "patches").string());
    proto::StagedFile *patch = ws.add_staged_files();
    patch->set_path("c-ok.diff");
    patch->set_content("a patch");
    ws.add_patch_files("c-ok.diff");
    ws.set_patch(proto::Workspace::PATCH_IN_ENTRYPOINT);
    ws.set_sandbox_work_dir("/workspace");

    proto::Mount *output_base = ws.add_mounts();
    output_base->set_kind(proto::Mount::VOLUME);
    output_base->set_source("arena-slot0-output_base");
    output_base->set_target("/output_base");
    return ws;
  }

  proto::Mount DiskCacheVolume() {
    proto::Mount disk_cache;
    disk_cache.set_kind(proto::Mount::VOLUME);
    disk_cache.set_source("arena-disk_cache");
    disk_cache.set_target("/disk_cache");
    return disk_cache;
  }

  proto::Isolation HardenedIsolation() {
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
  *build->add_mounts() = DiskCacheVolume();
  *build->add_argv() = Word("bazel", true);
  *build->add_argv() = Word("--output_base=/output_base", true);
  *build->add_argv() = Word("build", true);
  *build->add_argv() = Word("--disk_cache=/disk_cache", true);
  *build->add_argv() = Word("//solutions/c-ok:bot", false);

  const proto::JobResult result = engine_->Run(job, nullptr);
  ASSERT_EQ(result.status().code(), proto::Status::OK)
      << result.status().message();

  // Byte for byte what //game_arena/sandbox/worker's
  // WholeDockerRunArgvIsPinned expects. Not one bind mount: the tree, the
  // patches and scratch are the job's own volumes, the caches persistent
  // ones.
  EXPECT_EQ(RunArgvFor("saw-0-argv-1-build"),
            "docker run --rm --name saw-0-argv-1-build "
            "--cap-drop ALL --security-opt no-new-privileges --read-only "
            "--tmpfs /tmp:exec --memory 4096m --pids-limit 512 "
            "--network none "
            "--mount type=volume,source=saw-0-argv-1-ws,target=/workspace "
            "--mount type=volume,source=saw-0-argv-1-scratch,target=/sandbox "
            "--mount type=volume,source=arena-slot0-output_base,"
            "target=/output_base "
            "--mount type=volume,source=saw-0-argv-1-patches,"
            "target=/patches,readonly "
            "--mount type=volume,source=arena-disk_cache,target=/disk_cache "
            "--entrypoint /bin/sh fake-image:1");
}

TEST_F(ContainerEngineTest, TheWorkspaceIsLoadedThroughTheDaemonNotMounted) {
  proto::Job job;
  job.set_id("saw-0-load-1");
  job.set_log_dir((root_ / "logs").string());
  *job.mutable_workspace() = SlotWorkspace();
  *job.mutable_isolation() = HardenedIsolation();
  proto::Phase *phase = job.add_phases();
  phase->set_name("build");
  proto::Step *build = phase->mutable_foreground();
  build->set_name("build");
  build->set_applies_patches(true);
  *build->add_mounts() = DiskCacheVolume();
  *build->add_argv() = Word("./x", false);

  ASSERT_EQ(engine_->Run(job, nullptr).status().code(), proto::Status::OK);

  const std::string log = Log();
  // Fresh volumes for the job, then a loader that exists to be copied into:
  // the tree as a tar on stdin (`cp -`), the staged files from the staging
  // dir, both read by the docker *client* on this side.
  const auto volume = log.find("docker volume create saw-0-load-1-ws");
  const auto create = log.find(
      "docker create --name saw-0-load-1-load "
      "--network none "
      "--mount type=volume,source=saw-0-load-1-ws,"
      "target=/workspace "
      "--mount type=volume,source=saw-0-load-1-patches,"
      "target=/patches "
      "--mount type=volume,source=saw-0-load-1-scratch,"
      "target=/sandbox "
      "--mount type=volume,source=arena-slot0-output_"
      "base,target=/output_base "
      "--mount type=volume,source=arena-disk_cache,"
      "target=/disk_cache "
      "--entrypoint /bin/sh fake-image:1 -c chown -R "
      "0:0 /workspace /patches /sandbox\n"
      "chown 0:0 /output_base\n"
      "chown 0:0 /disk_cache\n");
  const auto tree = log.find("docker cp - saw-0-load-1-load:/workspace");
  const auto patches = log.find("docker cp " + (root_ / "patches").string() +
                                "/. saw-0-load-1-load:/patches");
  const auto start = log.find("docker start -a saw-0-load-1-load");
  const auto run = log.find("docker run --rm --name saw-0-load-1-build");
  // Removed at the end -- and cleared up front too, in case a killed job left
  // one behind, which is why this looks for the last removal.
  const auto removed = log.rfind("docker volume rm -f saw-0-load-1-ws");
  ASSERT_NE(volume, std::string::npos) << log;
  ASSERT_NE(create, std::string::npos) << log;
  ASSERT_NE(tree, std::string::npos) << log;
  ASSERT_NE(patches, std::string::npos) << log;
  ASSERT_NE(start, std::string::npos) << log;
  ASSERT_NE(run, std::string::npos) << log;
  ASSERT_NE(removed, std::string::npos) << log;
  EXPECT_LT(volume, create);
  EXPECT_LT(create, tree);
  EXPECT_LT(tree, patches);
  EXPECT_LT(patches, start);
  EXPECT_LT(start, run);
  EXPECT_LT(run, removed);
  EXPECT_EQ(log.find("type=bind"), std::string::npos) << log;
  // The tar itself is not left behind.
  EXPECT_FALSE(std::filesystem::exists(root_ / "logs" / "tree.tar"));
}

TEST_F(ContainerEngineTest, TheLoaderHandsTheTreeToTheSandboxUser) {
  proto::Job job;
  job.set_id("saw-0-user-1");
  job.set_log_dir((root_ / "logs").string());
  *job.mutable_workspace() = SlotWorkspace();
  *job.mutable_isolation() = HardenedIsolation();
  job.mutable_isolation()->set_run_as_user("1000:1000");
  proto::Phase *phase = job.add_phases();
  phase->set_name("build");
  proto::Step *build = phase->mutable_foreground();
  build->set_name("build");
  *build->add_mounts() = DiskCacheVolume();
  *build->add_argv() = Word("./x", false);

  ASSERT_EQ(engine_->Run(job, nullptr).status().code(), proto::Status::OK);

  // Copied-in files keep the worker's ownership; the loader's one job is to
  // chown them, and the (possibly fresh) persistent volumes' roots, to
  // whoever the steps run as. Not a bind mount's: that directory is the
  // host's.
  const std::string log = Log();
  EXPECT_NE(log.find("-c chown -R 1000:1000 /workspace /patches /sandbox\n"
                     "chown 1000:1000 /output_base\n"
                     "chown 1000:1000 /disk_cache\n"),
            std::string::npos)
      << log;
}

TEST_F(ContainerEngineTest, CollectedFilesAreCopiedOutOfTheKeptContainer) {
  // A fake docker whose `cp` out of a container writes a report.
  std::ofstream docker(root_ / "docker");
  docker << "#!/usr/bin/env bash\n"
         << "echo \"docker $*\" >> \"" << (root_ / "docker.log").string()
         << "\"\n"
         << "case \"$1\" in\n"
         << "  cp) case \"$2\" in *:/sandbox/report.json) "
            "echo '{\"metrics\":{\"x\":1}}' > \"$3\";; esac;;\n"
         << "esac\n"
         << "exit 0\n";
  docker.close();

  proto::Job job;
  job.set_id("saw-0-collect-1");
  job.set_log_dir((root_ / "logs").string());
  *job.mutable_workspace() = SlotWorkspace();
  *job.mutable_isolation() = HardenedIsolation();
  proto::Phase *phase = job.add_phases();
  phase->set_name("grade");
  proto::Step *grade = phase->mutable_foreground();
  grade->set_name("grade");
  grade->add_collect_files("report.json");
  *grade->add_argv() = Word("./bench", false);

  const proto::JobResult result = engine_->Run(job, nullptr);
  ASSERT_EQ(result.status().code(), proto::Status::OK);
  ASSERT_EQ(result.phases_size(), 1);
  const auto &collected = result.phases(0).steps(0).collected();
  ASSERT_EQ(collected.count("report.json"), 1u);
  EXPECT_NE(collected.at("report.json").find("\"x\":1"), std::string::npos);
  // Kept rather than --rm'd, so its scratch could still be asked for; then
  // removed with the rest.
  const std::string log = Log();
  EXPECT_EQ(RunArgvFor("saw-0-collect-1-grade").find("--rm"),
            std::string::npos);
  EXPECT_NE(log.find("docker cp saw-0-collect-1-grade:/sandbox/report.json"),
            std::string::npos)
      << log;
  EXPECT_NE(log.find("docker rm -f saw-0-collect-1-grade"), std::string::npos);
}

TEST_F(ContainerEngineTest, APrivateScratchIsThatStepsAlone) {
  proto::Job job;
  job.set_id("saw-0-priv-1");
  job.set_log_dir((root_ / "logs").string());
  *job.mutable_workspace() = SlotWorkspace();
  *job.mutable_isolation() = HardenedIsolation();
  job.mutable_isolation()->set_run_as_user("1000:1000");
  proto::Phase *phase = job.add_phases();
  phase->set_name("match");
  proto::Step *judge = phase->add_background();
  judge->set_name("judge");
  judge->set_private_scratch(true);
  judge->add_collect_files("verdict");
  *judge->add_argv() = Word("./judge", false);
  proto::Step *player = phase->mutable_foreground();
  player->set_name("player");
  *player->add_argv() = Word("./player", false);

  ASSERT_EQ(engine_->Run(job, nullptr).status().code(), proto::Status::OK);

  // Its own volume at the scratch mount point, where the other steps have the
  // job's shared one: nothing they write can end up in what it leaves.
  EXPECT_NE(RunArgvFor("saw-0-priv-1-judge")
                .find("source=saw-0-priv-1-judge-scratch,target=/sandbox "),
            std::string::npos);
  EXPECT_NE(RunArgvFor("saw-0-priv-1-player")
                .find("source=saw-0-priv-1-scratch,target=/sandbox "),
            std::string::npos);
  EXPECT_EQ(RunArgvFor("saw-0-priv-1-player").find("judge-scratch"),
            std::string::npos);
  // Made with the job, handed to the sandbox's user, collected from through
  // its own container, and removed with the job.
  const std::string log = Log();
  EXPECT_NE(log.find("docker volume create saw-0-priv-1-judge-scratch"),
            std::string::npos);
  EXPECT_NE(log.find("chown 1000:1000 /private_scratch/judge\n"),
            std::string::npos)
      << log;
  EXPECT_NE(log.find("docker cp saw-0-priv-1-judge:/sandbox/verdict"),
            std::string::npos);
  EXPECT_NE(log.find("docker volume rm -f saw-0-priv-1-judge-scratch"),
            std::string::npos);
}

TEST_F(ContainerEngineTest, TheLoaderLeavesReadOnlyMountsAlone) {
  proto::Job job;
  job.set_id("saw-0-ro-1");
  job.set_log_dir((root_ / "logs").string());
  *job.mutable_workspace() = SlotWorkspace();
  job.mutable_workspace()->clear_mounts();
  *job.mutable_isolation() = HardenedIsolation();
  proto::Mount cache = DiskCacheVolume();
  proto::Phase *build = job.add_phases();
  build->set_name("build");
  build->mutable_foreground()->set_name("build");
  *build->mutable_foreground()->add_mounts() = cache;
  *build->mutable_foreground()->add_argv() = Word("./build", false);
  proto::Phase *run = job.add_phases();
  run->set_name("run");
  cache.set_readonly(true);
  run->mutable_foreground()->set_name("run");
  *run->mutable_foreground()->add_mounts() = cache;
  *run->mutable_foreground()->add_argv() = Word("./run", false);

  ASSERT_EQ(engine_->Run(job, nullptr).status().code(), proto::Status::OK);

  // The same volume, writable in one step and read-only in the next, reaches
  // the loader once: docker refuses a second mount at one target.
  const std::string log = Log();
  const std::size_t loader = log.find("docker create --name saw-0-ro-1-load");
  ASSERT_NE(loader, std::string::npos);
  const std::string line =
      log.substr(loader, log.find(" -c ", loader) - loader);
  const std::size_t first = line.find("target=/disk_cache");
  ASSERT_NE(first, std::string::npos) << line;
  EXPECT_EQ(line.find("target=/disk_cache", first + 1), std::string::npos)
      << line;
  EXPECT_NE(RunArgvFor("saw-0-ro-1-run").find("target=/disk_cache,readonly"),
            std::string::npos);
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
  EXPECT_NE(log.find("docker network rm saw-0-ok-1-net"), std::string::npos);
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
    EXPECT_EQ(line.find("type=bind"), std::string::npos) << line;
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

TEST_F(ContainerEngineTest, AJobNeedsAnImageToLoadWith) {
  proto::Job job;
  job.set_id("noimage");
  job.set_log_dir((root_ / "logs").string());
  *job.mutable_workspace() = SlotWorkspace();
  proto::Phase *phase = job.add_phases();
  phase->set_name("build");
  *phase->mutable_foreground()->add_argv() = Word("./x", false);
  const proto::JobResult result = engine_->Run(job, nullptr);
  EXPECT_EQ(result.status().code(), proto::Status::INVALID_JOB);
  EXPECT_EQ(Log().find("docker"), std::string::npos);
}

TEST_F(ContainerEngineTest, CancelIsANoOpForAJobItIsNotRunning) {
  engine_->Cancel("never-heard-of-it");
  EXPECT_EQ(Log().find("kill"), std::string::npos);
}

}  // namespace
}  // namespace sandbox_exec
