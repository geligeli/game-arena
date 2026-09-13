// What an order becomes. The shape of the job, not the bytes docker sees --
// those are pinned end to end in order_runner_container_test.

#include "game_arena/sandbox/worker/order_job.h"

#include <string>

#include "game_arena/sandbox/exec/process_engine.h"
#include "gtest/gtest.h"

namespace tournament_arena {
namespace {

namespace sx = sandbox_exec::proto;

sandbox_exec::Capabilities ContainerCapabilities() {
  return sandbox_exec::Capabilities{/*isolates=*/true, /*shared_network=*/true,
                                    /*stable_peer_names=*/true};
}

sandbox_exec::Capabilities ProcessCapabilities() {
  return sandbox_exec::ProcessEngine().capabilities();
}

OrderJobConfig Config() {
  OrderJobConfig config;
  config.work_dir = "/w";
  config.disk_cache = "/w/disk_cache";
  return config;
}

proto::WorkOrder MatchOrder() {
  proto::WorkOrder order;
  order.set_order_id("ok-1");
  order.set_game("nim");
  order.set_base_commit("abc123");
  order.set_referee_target("//testgame:match_referee");
  order.set_opponent_spec("builtin:random");
  order.set_num_games(2);
  order.set_repo_url("/repo");
  proto::SandboxOrder *sandbox = order.mutable_sandbox();
  sandbox->set_image("img:1");
  sandbox->set_memory_limit_mb(4096);
  sandbox->set_pids_limit(512);

  proto::Side *side = order.mutable_candidate();
  side->set_candidate_id("c-ok");
  side->set_patch("a patch");
  side->add_build_targets("//solutions/c-ok:bot");
  side->set_bot_target("//solutions/c-ok:bot");
  return order;
}

std::string ArgvOf(const sx::Step &step) {
  std::string joined;
  for (const sx::Token &token : step.argv()) {
    if (!joined.empty()) {
      joined += " ";
    }
    joined += token.text();
  }
  return joined;
}

TEST(JobForOrderTest, AMatchIsABuildPhaseThenAMatchPhase) {
  sx::Job job;
  std::string error;
  ASSERT_TRUE(JobForOrder(0, MatchOrder(), Config(), ContainerCapabilities(),
                          &job, &error))
      << error;

  ASSERT_EQ(job.phases_size(), 2);
  EXPECT_EQ(job.phases(0).name(), "build");
  EXPECT_EQ(job.phases(1).name(), "match");
  // The referee runs beside the bot, not in front of it.
  EXPECT_EQ(job.phases(1).background_size(), 1);
  EXPECT_EQ(job.phases(1).background(0).name(), "referee");
  EXPECT_EQ(job.phases(1).foreground().name(), "bot");
  EXPECT_EQ(job.id(), "saw-0-ok-1");
}

TEST(JobForOrderTest, TheBuildReachesNothingAndTheMatchReachesOnlyItself) {
  sx::Job job;
  std::string error;
  ASSERT_TRUE(JobForOrder(0, MatchOrder(), Config(), ContainerCapabilities(),
                          &job, &error));

  EXPECT_EQ(job.phases(0).isolation().network(), sx::Isolation::NETWORK_NONE);
  EXPECT_EQ(job.phases(1).isolation().network(),
            sx::Isolation::NETWORK_PHASE_BRIDGE);
}

TEST(JobForOrderTest, OnlyTheBuildSeesThePatchesAndTheCache) {
  sx::Job job;
  std::string error;
  ASSERT_TRUE(JobForOrder(0, MatchOrder(), Config(), ContainerCapabilities(),
                          &job, &error));

  // The thing the build produced has no business reading the shared cache
  // directory or writing the shared build cache.
  const sx::Step &build = job.phases(0).foreground();
  ASSERT_EQ(build.mounts_size(), 1);
  EXPECT_EQ(build.mounts(0).target(), "/disk_cache");
  EXPECT_EQ(job.phases(1).foreground().mounts_size(), 0);
  // The output base is every step's, so it is on the workspace.
  ASSERT_EQ(job.workspace().mounts_size(), 1);
  EXPECT_EQ(job.workspace().mounts(0).target(), "/output_base");
}

TEST(JobForOrderTest, OneBuildBuildsEverySideAndTheReferee) {
  proto::WorkOrder order = MatchOrder();
  proto::Side *opponent = order.mutable_opponent();
  opponent->set_candidate_id("c-rival");
  opponent->set_patch("another patch");
  opponent->add_build_targets("//solutions/c-rival:bot");
  opponent->set_bot_target("//solutions/c-rival:bot");

  sx::Job job;
  std::string error;
  ASSERT_TRUE(
      JobForOrder(0, order, Config(), ContainerCapabilities(), &job, &error));

  // One analysis pass and, more importantly, one consistent tree.
  const std::string argv = ArgvOf(job.phases(0).foreground());
  EXPECT_NE(argv.find("//solutions/c-ok:bot"), std::string::npos) << argv;
  EXPECT_NE(argv.find("//solutions/c-rival:bot"), std::string::npos) << argv;
  EXPECT_NE(argv.find("//testgame:match_referee"), std::string::npos) << argv;
  // Both patches are staged, and the opponent plays from the background.
  EXPECT_EQ(job.workspace().staged_files_size(), 2);
  EXPECT_EQ(job.phases(1).background_size(), 2);
  EXPECT_EQ(job.phases(1).background(1).name(), "opponent");
}

TEST(JobForOrderTest, AContainerRefereeListensOnAFixedPortAndIsDialledByName) {
  sx::Job job;
  std::string error;
  ASSERT_TRUE(JobForOrder(0, MatchOrder(), Config(), ContainerCapabilities(),
                          &job, &error));

  // Its own network namespace, so nothing can collide on the port and the
  // peer resolves by name.
  EXPECT_EQ(job.phases(1).background(0).endpoint().port(), 50051);
  EXPECT_NE(ArgvOf(job.phases(1).background(0)).find("--port=50051"),
            std::string::npos);
  EXPECT_NE(ArgvOf(job.phases(1).foreground())
                .find("--server=saw-0-ok-1-referee:50051"),
            std::string::npos)
      << ArgvOf(job.phases(1).foreground());
}

TEST(JobForOrderTest, AProcessRefereeBindsPortZeroAndPublishesIt) {
  sx::Job job;
  std::string error;
  ASSERT_TRUE(JobForOrder(0, MatchOrder(), Config(), ProcessCapabilities(),
                          &job, &error));

  // Parallel slots share one host, so a fixed port would collide. This is the
  // one place the two engines genuinely differ rather than merely having
  // drifted.
  const sx::Step &referee = job.phases(1).background(0);
  EXPECT_TRUE(referee.endpoint().discover_via_port_file());
  const std::string argv = ArgvOf(referee);
  EXPECT_NE(argv.find("--port=0"), std::string::npos) << argv;
  EXPECT_NE(argv.find("--port_file={{port_file}}"), std::string::npos) << argv;
  EXPECT_NE(argv.find("--scratch_dir={{scratch}}"), std::string::npos) << argv;
  EXPECT_NE(
      ArgvOf(job.phases(1).foreground()).find("--server={{peer:referee}}"),
      std::string::npos);
}

TEST(JobForOrderTest, RegistryOptionsAreForwardedVerbatimOrOmitted) {
  proto::WorkOrder order = MatchOrder();
  sx::Job without;
  std::string error;
  ASSERT_TRUE(JobForOrder(0, order, Config(), ContainerCapabilities(), &without,
                          &error));
  EXPECT_EQ(ArgvOf(without.phases(1).background(0)).find("--registry_options"),
            std::string::npos);

  (*order.mutable_registry_options())["iterations"] = "100";
  sx::Job with;
  ASSERT_TRUE(
      JobForOrder(0, order, Config(), ContainerCapabilities(), &with, &error));
  EXPECT_NE(ArgvOf(with.phases(1).background(0))
                .find("--registry_options=iterations=100"),
            std::string::npos);
}

TEST(JobForOrderTest, AContainerNeedsNoCapabilities) {
  sx::Job job;
  std::string error;
  ASSERT_TRUE(JobForOrder(0, MatchOrder(), Config(), ContainerCapabilities(),
                          &job, &error));

  // Nothing to mount, so nothing to be privileged for: the tree arrives in a
  // volume and the defaults stand.
  EXPECT_FALSE(job.isolation().keep_default_caps());
  EXPECT_FALSE(job.isolation().writable_rootfs());
  EXPECT_FALSE(job.isolation().allow_new_privileges());
  EXPECT_EQ(job.isolation().add_capabilities_size(), 0);
  ASSERT_EQ(job.isolation().tmpfs_size(), 1);
  EXPECT_EQ(job.isolation().tmpfs(0).target(), "/tmp");
  EXPECT_EQ(job.workspace().patch(), sx::Workspace::PATCH_IN_ENTRYPOINT);
  EXPECT_EQ(job.workspace().tree_dir(), "/w/slot0/repo");
}

TEST(JobForOrderTest, PersistentStateLivesInVolumesUnlessTheHostBindsIt) {
  sx::Job job;
  std::string error;
  ASSERT_TRUE(JobForOrder(1, MatchOrder(), Config(), ContainerCapabilities(),
                          &job, &error));
  // Named per slot and per worker, so two slots never share an output base
  // and two workers on one daemon can be told apart.
  ASSERT_EQ(job.workspace().mounts_size(), 1);
  EXPECT_EQ(job.workspace().mounts(0).kind(), sx::Mount::VOLUME);
  EXPECT_EQ(job.workspace().mounts(0).source(), "arena-slot1-output_base");
  const sx::Step &build = job.phases(0).foreground();
  ASSERT_EQ(build.mounts_size(), 1);
  EXPECT_EQ(build.mounts(0).kind(), sx::Mount::VOLUME);
  EXPECT_EQ(build.mounts(0).source(), "arena-disk_cache");

  // The opt-in: host directories, as the daemon resolves them.
  OrderJobConfig bound = Config();
  bound.volume_prefix = "w2";
  bound.bind_output_base_dir = "/fast/output_bases";
  bound.bind_disk_cache_dir = "/fast/disk_cache";
  sx::Job bound_job;
  ASSERT_TRUE(JobForOrder(1, MatchOrder(), bound, ContainerCapabilities(),
                          &bound_job, &error));
  EXPECT_EQ(bound_job.workspace().mounts(0).kind(), sx::Mount::BIND);
  EXPECT_EQ(bound_job.workspace().mounts(0).source(),
            "/fast/output_bases/slot1");
  EXPECT_EQ(bound_job.phases(0).foreground().mounts(0).kind(), sx::Mount::BIND);
  EXPECT_EQ(bound_job.phases(0).foreground().mounts(0).source(),
            "/fast/disk_cache");
}

TEST(JobForOrderTest, TheBuildCarriesNoMemoryOrPidCap) {
  sx::Job job;
  std::string error;
  ASSERT_TRUE(JobForOrder(0, MatchOrder(), Config(), ContainerCapabilities(),
                          &job, &error));
  // The problem's caps bound its solution; bazel's JVM and a few dozen
  // compilers are the problem's own toolchain, bounded by the build timeout.
  const sx::Isolation &build = job.phases(0).isolation();
  EXPECT_EQ(build.memory_limit_mb(), 0u);
  EXPECT_EQ(build.pids_limit(), 0u);
  EXPECT_EQ(build.image(), "img:1");
  EXPECT_EQ(job.phases(1).isolation().memory_limit_mb(), 4096u);
  EXPECT_EQ(job.phases(1).isolation().pids_limit(), 512u);
}

TEST(JobForOrderTest, AGradedOrderIsOnePhasePerRun) {
  proto::WorkOrder order = MatchOrder();
  order.clear_referee_target();
  proto::GradeOrder *grade = order.mutable_grade();
  grade->add_argv("./bench");
  grade->set_repeats(3);
  grade->add_metric_names("wall_ms");

  sx::Job job;
  std::string error;
  ASSERT_TRUE(
      JobForOrder(0, order, Config(), ContainerCapabilities(), &job, &error));

  // A build, then one phase per measurement run: a run that hangs must not be
  // waited on by the next.
  ASSERT_EQ(job.phases_size(), 4);
  for (int run = 1; run < 4; ++run) {
    const sx::Step &step = job.phases(run).foreground();
    EXPECT_EQ(job.phases(run).name(), "grade");
    EXPECT_EQ(step.env().at("ARENA_REPORT"), "{{scratch}}/report.json");
    ASSERT_EQ(step.collect_files_size(), 1);
    EXPECT_EQ(step.collect_files(0), "report.json");
    // A solution timed against a stopwatch has no business reaching the
    // network.
    EXPECT_EQ(job.phases(run).isolation().network(),
              sx::Isolation::NETWORK_NONE);
  }
}

TEST(JobForOrderTest, TimeoutsFallBackToTheSameDefaultsTheBackendsUsed) {
  proto::WorkOrder order = MatchOrder();
  order.set_run_timeout_s(100);

  sx::Job job;
  std::string error;
  ASSERT_TRUE(
      JobForOrder(0, order, Config(), ContainerCapabilities(), &job, &error));

  EXPECT_EQ(job.phases(0).foreground().timeout_s(), 1800);
  EXPECT_EQ(job.phases(1).foreground().timeout_s(), 100);
  // Bounded below the run timeout, so a stuck match yields a partial tally
  // rather than an order-level failure.
  EXPECT_NE(ArgvOf(job.phases(1).background(0)).find("--deadline_s=70"),
            std::string::npos)
      << ArgvOf(job.phases(1).background(0));
}

TEST(JobForOrderTest, ASideWithoutAPatchIsRejected) {
  proto::WorkOrder order = MatchOrder();
  order.mutable_candidate()->clear_patch();

  sx::Job job;
  std::string error;
  EXPECT_FALSE(
      JobForOrder(0, order, Config(), ContainerCapabilities(), &job, &error));
  EXPECT_NE(error.find("carries no patch"), std::string::npos) << error;
}

TEST(JobForOrderTest, WithoutASandboxThePatchIsAppliedOnTheHost) {
  sx::Job job;
  std::string error;
  ASSERT_TRUE(JobForOrder(0, MatchOrder(), Config(), ProcessCapabilities(),
                          &job, &error));

  EXPECT_EQ(job.workspace().patch(), sx::Workspace::PATCH_HOST);
  EXPECT_EQ(job.workspace().scratch_dir(), "/w/slot0/scratch");
  EXPECT_TRUE(job.isolation().image().empty());
}

TEST(JobForOrderTest, TheMemoryCapIsOnTheSolutionAndNotOnTheBuild) {
  sx::Job job;
  std::string error;
  ASSERT_TRUE(JobForOrder(0, MatchOrder(), Config(), ProcessCapabilities(),
                          &job, &error));

  // A cap on the build is a cap on bazel, whose JVM reserves far more address
  // space than any limit a problem means for a solution -- it dies at
  // startup. The old local backend passed 0 here for exactly this reason, and
  // hoisting the limit to the job put it back.
  EXPECT_EQ(job.phases(0).foreground().isolation().address_space_limit_bytes(),
            0u);
  EXPECT_EQ(job.isolation().address_space_limit_bytes(), 0u);
  EXPECT_GT(job.phases(1).foreground().isolation().address_space_limit_bytes(),
            0u);
}

TEST(JobForOrderTest, AContainersMemoryLimitIsACgroupNotAnAddressSpaceCap) {
  sx::Job job;
  std::string error;
  ASSERT_TRUE(JobForOrder(0, MatchOrder(), Config(), ContainerCapabilities(),
                          &job, &error));

  EXPECT_EQ(job.isolation().memory_limit_mb(), 4096u);
  EXPECT_EQ(job.isolation().address_space_limit_bytes(), 0u);
}

}  // namespace
}  // namespace tournament_arena
