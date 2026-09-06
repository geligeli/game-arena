// The pure helpers of the docker backend: shell quoting, container-name
// sanitisation, and the exact /bin/sh scripts the containers would run.
// Asserted directly, without a docker daemon.

#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "game_mcts/tournament_server/sandbox_worker/docker_backend.h"

namespace tournament_arena {
namespace {

TEST(ShellQuoteTest, QuotesEverything) {
  EXPECT_EQ(ShellQuote("plain"), "'plain'");
  EXPECT_EQ(ShellQuote("a b"), "'a b'");
  EXPECT_EQ(ShellQuote("it's"), "'it'\\''s'");
  EXPECT_EQ(ShellQuote(""), "''");
  EXPECT_EQ(ShellQuote("$HOME `id` \"x\""), "'$HOME `id` \"x\"'");
}

TEST(SanitizeContainerNameTest, ReplacesInvalidCharacters) {
  // Docker names: [a-zA-Z0-9][a-zA-Z0-9_.-]*
  EXPECT_EQ(SanitizeContainerName("order-1.a_b"), "order-1.a_b");
  EXPECT_EQ(SanitizeContainerName("order/1: x"), "order-1--x");
  EXPECT_EQ(SanitizeContainerName("a b"), "a-b");
}

TEST(DockerBuildScriptTest, MountsOverlayAndExecsBazel) {
  const std::string script = DockerBuildScript("/cache", {"--config=native"},
                                               "//game_mcts/x:c-1:bot");
  EXPECT_NE(script.find("set -eu"), std::string::npos);
  // One overlay assembly, upper/work under the single scratch mount.
  EXPECT_NE(
      script.find("mkdir -p /sandbox/upper /sandbox/work /workspace"),
      std::string::npos);
  EXPECT_NE(script.find("mount -t overlay overlay -o lowerdir=/repo_lower,"
                        "upperdir=/sandbox/upper,workdir=/sandbox/work "
                        "/workspace"),
            std::string::npos);
  // The staged candidate is copied over the merge before anything builds.
  EXPECT_NE(script.find("cp -a /patches/. /workspace/"), std::string::npos);
  // The build execs bazel against the persistent slot output base, so a warm
  // slot compiles only the submitted files.
  EXPECT_NE(
      script.find("exec bazel --output_base=/output_base "
                  "--disk_cache=/disk_cache '--config=native' "
                  "build '//game_mcts/x:c-1:bot'"),
      std::string::npos);
}

TEST(DockerBuildScriptTest, OmitsDiskCacheWhenUnset) {
  const std::string script =
      DockerBuildScript("", {}, "//x:y");
  EXPECT_EQ(script.find("disk_cache"), std::string::npos);
  EXPECT_NE(script.find("exec bazel --output_base=/output_base build '//x:y'"),
            std::string::npos);
}

TEST(DockerRunScriptTest, ExecsTheBuiltBotWithQuotedArgs) {
  const std::string script =
      DockerRunScript("./bazel-bin/game_mcts/x/c-1/bot",
                      {"--name=c-1", "--server=h:1",
                       "--opponent=player:o t h e r s", "--games=2",
                       "--params=a=1,b=2"});
  EXPECT_NE(script.find("set -eu"), std::string::npos);
  EXPECT_NE(
      script.find("exec './bazel-bin/game_mcts/x/c-1/bot' '--name=c-1' "
                  "'--server=h:1' '--opponent=player:o t h e r s' "
                  "'--games=2' '--params=a=1,b=2'"),
      std::string::npos);
  // No build happens in the run phase: the bot comes from the shared output
  // base through the workspace's bazel-bin symlink.
  EXPECT_EQ(script.find("bazel "), std::string::npos);
}

}  // namespace
}  // namespace tournament_arena
