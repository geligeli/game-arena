#include "game_arena/tools/sandbox_priming.h"

#include <fstream>
#include <sstream>

#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace tournament_arena {
namespace {

using ::testing::HasSubstr;

proto::ProblemConfig Config() {
  proto::ProblemConfig config;
  config.mutable_build()->add_targets("//bots/{submission_id}:bot");
  config.mutable_build()->add_targets("//:harness");
  config.mutable_match()->set_referee_target("//:match_referee");
  return config;
}

TEST(PrimeTargetsTest, ExpandsTheSubmission) {
  EXPECT_EQ(PrimeTargets(Config(), "reference"),
            (std::vector<std::string>{"//bots/reference:bot", "//:harness",
                                      "//:match_referee"}));
}

TEST(PrimeTargetsTest, NoSubmissionLeavesItsTargetsOut) {
  EXPECT_EQ(PrimeTargets(Config(), ""),
            (std::vector<std::string>{"//:harness", "//:match_referee"}));
}

// The rc a sandbox reads, re-rooted: if it stops carrying what makes a primed
// cache hit, priming quietly ships a cache nothing uses.
TEST(PrimeBazelrcTest, ReRootsTheImagesRc) {
  std::ifstream in("game_arena/image/sandbox.bazelrc");
  std::stringstream rc;
  rc << in.rdbuf();
  ASSERT_FALSE(rc.str().empty());

  const std::string primed = PrimeBazelrc(rc.str(), "/stage/root");
  EXPECT_THAT(primed, HasSubstr("--override_module=game_arena=/stage/root/opt/"
                                "arena/src/game_arena\n"));
  EXPECT_THAT(primed, HasSubstr("try-import /stage/root/opt/arena/primed"));
  EXPECT_THAT(primed, HasSubstr("build --incompatible_strict_action_env\n"));
  EXPECT_THAT(primed, ::testing::EndsWith("try-import %workspace%/.bazelrc\n"));
}

TEST(TarOwnerTest, FromRunAsUser) {
  EXPECT_EQ(TarOwner(""), (std::vector<std::string>{"--owner=0", "--group=0"}));
  EXPECT_EQ(TarOwner("1000:1000"),
            (std::vector<std::string>{"--owner=1000", "--group=1000"}));
  EXPECT_EQ(TarOwner("1000"),
            (std::vector<std::string>{"--owner=1000", "--group=1000"}));
}

}  // namespace
}  // namespace tournament_arena
