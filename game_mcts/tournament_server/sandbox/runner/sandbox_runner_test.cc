#include "game_mcts/tournament_server/sandbox/runner/sandbox_runner.h"

#include <gtest/gtest.h>

namespace sandbox_runner {
namespace {

TEST(IsSafePatchPathTest, AcceptsRelativePaths) {
  EXPECT_TRUE(IsSafePatchPath("BUILD"));
  EXPECT_TRUE(IsSafePatchPath("game_mcts/core/mcts/mcts.h"));
  EXPECT_TRUE(IsSafePatchPath("a b/c-d_e.f"));
}

TEST(IsSafePatchPathTest, RejectsEscapesAndEmpty) {
  EXPECT_FALSE(IsSafePatchPath(""));
  EXPECT_FALSE(IsSafePatchPath("/etc/passwd"));
  EXPECT_FALSE(IsSafePatchPath("../outside"));
  EXPECT_FALSE(IsSafePatchPath("a/../../b"));
  EXPECT_FALSE(IsSafePatchPath("./BUILD"));
  EXPECT_FALSE(IsSafePatchPath("a/./b"));
}

// Shell quoting and the name-sanitising alphabet are tested in
// sandbox/common/docker_test.cc; here only the runner's prefix.

TEST(ContainerNameTest, StaysWithinDockerAlphabet) {
  EXPECT_EQ(ContainerName("order-123"), "sbr-order-123");
  EXPECT_EQ(ContainerName("a/b c:d"), "sbr-a-b-c-d");
  EXPECT_EQ(ContainerName("with_underscore.and.dot"),
            "sbr-with_underscore.and.dot");
}

TEST(ContainerNameTest, StartsAlphanumericEvenForOddIdentifiers) {
  const std::string name = ContainerName("///");
  EXPECT_TRUE(std::isalnum(static_cast<unsigned char>(name.front())) != 0);
}

}  // namespace
}  // namespace sandbox_runner
