// The flags, and their order. Lifted from the hardening this replaces, so a
// difference here is a difference in what a sandbox is allowed to do.

#include "game_arena/sandbox/exec/isolation.h"

#include <string>
#include <vector>

#include "gtest/gtest.h"

namespace sandbox_exec {
namespace {

TEST(IsolationArgsTest, ADefaultIsolationIsTheHardenedOne) {
  // The point of the relaxation polarity: a caller that sets nothing, or one
  // written before a field was added, gets the tight sandbox.
  EXPECT_EQ(IsolationArgs(proto::Isolation()),
            std::vector<std::string>({"--cap-drop", "ALL", "--security-opt",
                                      "no-new-privileges", "--read-only"}));
}

TEST(IsolationArgsTest, ReproducesTheWorkersHardenedContainer) {
  proto::Isolation isolation;
  isolation.set_memory_limit_mb(4096);
  isolation.set_pids_limit(512);
  proto::Tmpfs *tmpfs = isolation.add_tmpfs();
  tmpfs->set_target("/tmp");
  tmpfs->set_options("exec");

  EXPECT_EQ(IsolationArgs(isolation),
            std::vector<std::string>({"--cap-drop", "ALL", "--security-opt",
                                      "no-new-privileges", "--read-only",
                                      "--tmpfs", "/tmp:exec", "--memory",
                                      "4096m", "--pids-limit", "512"}));
}

TEST(IsolationArgsTest, ReproducesTheInSandboxOverlayFallback) {
  // The mode that needs CAP_SYS_ADMIN: today's code emits only the cap-add
  // and keeps docker's whole default capability set.
  proto::Isolation isolation;
  isolation.set_keep_default_caps(true);
  isolation.set_allow_new_privileges(true);
  isolation.set_writable_rootfs(true);
  isolation.add_add_capabilities("SYS_ADMIN");
  isolation.set_memory_limit_mb(4096);
  isolation.set_pids_limit(512);

  EXPECT_EQ(IsolationArgs(isolation),
            std::vector<std::string>({"--cap-add", "SYS_ADMIN", "--memory",
                                      "4096m", "--pids-limit", "512"}));
}

TEST(IsolationArgsTest, ACapabilityCanBeAddedBackOnTopOfTheDrop) {
  // Tighter than keep_default_caps: everything dropped, one granted.
  proto::Isolation isolation;
  isolation.add_add_capabilities("SYS_ADMIN");
  isolation.set_writable_rootfs(true);

  EXPECT_EQ(IsolationArgs(isolation),
            std::vector<std::string>({"--cap-drop", "ALL", "--security-opt",
                                      "no-new-privileges", "--cap-add",
                                      "SYS_ADMIN"}));
}

TEST(IsolationArgsTest, UserAndCpusAppearOnlyWhenSet) {
  proto::Isolation isolation;
  isolation.set_writable_rootfs(true);
  isolation.set_keep_default_caps(true);
  isolation.set_allow_new_privileges(true);
  EXPECT_TRUE(IsolationArgs(isolation).empty());

  isolation.set_run_as_user("1000:1000");
  isolation.set_cpus(2.5);
  const std::vector<std::string> args = IsolationArgs(isolation);
  EXPECT_EQ(args[0], "--user");
  EXPECT_EQ(args[1], "1000:1000");
  EXPECT_EQ(args[2], "--cpus");
}

TEST(NetworkArgTest, NoNetworkIsTheDefaultAndDockersDefaultIsNeverUsed) {
  EXPECT_EQ(NetworkArg(proto::Isolation(), "phase-net"), "none");
}

TEST(NetworkArgTest, APhaseBridgeIsNamedAfterThePhase) {
  proto::Isolation isolation;
  isolation.set_network(proto::Isolation::NETWORK_PHASE_BRIDGE);
  EXPECT_EQ(NetworkArg(isolation, "saw-0-ok-1-net"), "saw-0-ok-1-net");
  EXPECT_TRUE(NeedsPhaseNetwork(isolation));
}

TEST(NetworkArgTest, EgressIsDeliberateAndNamed) {
  proto::Isolation isolation;
  isolation.set_network(proto::Isolation::NETWORK_EGRESS);
  EXPECT_EQ(NetworkArg(isolation, "unused"), "bridge");
  EXPECT_FALSE(NeedsPhaseNetwork(isolation));
}

}  // namespace
}  // namespace sandbox_exec
