// The order-specific entrypoint scripts: the exact /bin/sh text the build,
// run and grade containers would execute. Asserted directly, without a docker
// daemon. Shell quoting, name sanitisation, bind-mount syntax and the overlay
// prelude fragments are tested in sandbox/common/docker_test.cc.

#include "game_arena/sandbox/worker/docker_backend.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace tournament_arena {
namespace {

// These assert the in-container overlay form explicitly. It is the fallback
// now -- a host-mounted overlay leaves nothing for the script to assemble --
// and it is the one with a script worth asserting on.
TEST(DockerBuildScriptTest, MountsOverlayAndExecsBazel) {
  const std::string script = DockerBuildScript(
      "/cache", {"--config=native"}, {"c-1.diff"}, {"//problem/x:c-1:bot"},
      /*mount_in_container=*/true);
  EXPECT_NE(script.find("set -eu"), std::string::npos);
  // One overlay assembly, upper/work under the single scratch mount.
  EXPECT_NE(script.find("mkdir -p /sandbox/upper /sandbox/work /workspace"),
            std::string::npos);
  EXPECT_NE(script.find("mount -t overlay overlay -o lowerdir=/repo_lower,"
                        "upperdir=/sandbox/upper,workdir=/sandbox/work "
                        "/workspace"),
            std::string::npos);
  // The submission is applied by git, inside the container, before anything
  // builds. set -eu means a patch that does not apply aborts here with git's
  // own message rather than becoming a confusing compile error later.
  EXPECT_NE(script.find("git apply '/patches/c-1.diff'"), std::string::npos)
      << script;
  // The build execs bazel against the persistent slot output base, so a warm
  // slot compiles only the submitted files.
  EXPECT_NE(script.find("exec bazel --output_base=/output_base "
                        "--disk_cache=/disk_cache '--config=native' "
                        "build '//problem/x:c-1:bot'"),
            std::string::npos);
}

// One build, every target: both sides of a match and the referee share an
// analysis pass and, more importantly, one consistent tree.
TEST(DockerBuildScriptTest, AppliesEverySidesPatchBeforeBuilding) {
  const std::string script =
      DockerBuildScript("", {}, {"alpha.diff", "beta.diff"}, {"//a:bot"},
                        /*mount_in_container=*/true);
  const auto alpha = script.find("git apply '/patches/alpha.diff'");
  const auto beta = script.find("git apply '/patches/beta.diff'");
  const auto build = script.find("build '//a:bot'");
  ASSERT_NE(alpha, std::string::npos) << script;
  ASSERT_NE(beta, std::string::npos) << script;
  // Both sides land before bazel runs: a match builds one tree, not two.
  EXPECT_LT(alpha, build);
  EXPECT_LT(beta, build);
}

TEST(DockerBuildScriptTest, BuildsEveryTargetInOnePass) {
  const std::string script =
      DockerBuildScript("", {}, {"a.diff", "b.diff"},
                        {"//a:bot", "//b:bot", "//referee:match_referee"},
                        /*mount_in_container=*/true);
  EXPECT_NE(script.find("build '//a:bot' '//b:bot' '//referee:match_referee'"),
            std::string::npos)
      << script;
}

TEST(DockerBuildScriptTest, OmitsDiskCacheWhenUnset) {
  const std::string script = DockerBuildScript("", {}, {"p.diff"}, {"//x:y"},
                                               /*mount_in_container=*/true);
  EXPECT_EQ(script.find("disk_cache"), std::string::npos);
  EXPECT_NE(script.find("exec bazel --output_base=/output_base build '//x:y'"),
            std::string::npos);
}

TEST(DockerRunScriptTest, ExecsTheBuiltBotWithQuotedArgs) {
  const std::string script = DockerRunScript(
      "./bazel-bin/problem/x/c-1/bot",
      {"--name=c-1", "--server=h:1", "--opponent=player:o t h e r s",
       "--games=2", "--params=a=1,b=2"},
      /*mount_in_container=*/true);
  EXPECT_NE(script.find("set -eu"), std::string::npos);
  EXPECT_NE(script.find("exec './bazel-bin/problem/x/c-1/bot' '--name=c-1' "
                        "'--server=h:1' '--opponent=player:o t h e r s' "
                        "'--games=2' '--params=a=1,b=2'"),
            std::string::npos);
  // No build happens in the run phase: the bot comes from the shared output
  // base through the workspace's bazel-bin symlink.
  EXPECT_EQ(script.find("bazel "), std::string::npos);
}

// With the overlay already mounted on the host there is nothing to assemble,
// and nothing that would need CAP_SYS_ADMIN.
TEST(DockerBuildScriptTest, HostMountedOverlayNeedsNoMountInside) {
  const std::string script = DockerBuildScript("", {}, {"p.diff"}, {"//x:y"},
                                               /*mount_in_container=*/false);
  EXPECT_EQ(script.find("mount -t overlay"), std::string::npos) << script;
  // It still needs a writable HOME: bazel insists on one, and the container's
  // root filesystem is read-only.
  EXPECT_NE(script.find("export HOME=/sandbox"), std::string::npos) << script;
  EXPECT_NE(script.find("git apply '/patches/p.diff'"), std::string::npos);
  EXPECT_NE(script.find("build '//x:y'"), std::string::npos);
}

}  // namespace
}  // namespace tournament_arena
