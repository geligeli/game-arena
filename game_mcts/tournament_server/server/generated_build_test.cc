#include "game_mcts/tournament_server/server/generated_build.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace tournament_arena {
namespace {

constexpr char kDir[] = "game_mcts/tournament_server/candidates";

auto Build(const std::vector<std::string> &files,
           const std::string &entry = "strategy.h",
           const std::vector<std::string> &deps = {},
           const std::string &game = "risk2") -> std::string {
  return GenerateCandidateBuild(kDir, "c-1", game, files, entry, deps);
}

TEST(CandidateTargetTest, LabelsTheGeneratedBinary) {
  EXPECT_EQ(CandidateTarget(kDir, "my-bot-abc123"),
            "//game_mcts/tournament_server/candidates/my-bot-abc123:bot");
}

TEST(GenerateCandidateBuildTest, WiresTheEntryHeaderAndGameIntoTheBinary) {
  const std::string build = Build({"strategy.h"});
  EXPECT_NE(build.find("name = \"strategy\""), std::string::npos) << build;
  EXPECT_NE(build.find("name = \"bot\""), std::string::npos) << build;
  // The harness reaches the submission through this define, not a dep: the game
  // and the entry header are local_defines, which do not reach a prebuilt lib.
  EXPECT_NE(build.find("CANDIDATE_ENTRY_HEADER=\\\"game_mcts/tournament_server/"
                       "candidates/c-1/strategy.h\\\""),
            std::string::npos)
      << build;
  EXPECT_NE(build.find("CANDIDATE_GAME_RISK2"), std::string::npos) << build;
}

TEST(GenerateCandidateBuildTest, SeparatesHeadersFromCompiledSources) {
  const std::string build = Build({"strategy.h", "helper.cc", "helper.h"});
  const auto hdrs = build.find("hdrs = [");
  const auto srcs = build.find("srcs = [");
  ASSERT_NE(hdrs, std::string::npos) << build;
  ASSERT_NE(srcs, std::string::npos) << build;
  const std::string hdrs_block = build.substr(hdrs, srcs - hdrs);
  EXPECT_NE(hdrs_block.find("strategy.h"), std::string::npos) << build;
  EXPECT_NE(hdrs_block.find("helper.h"), std::string::npos) << build;
  EXPECT_EQ(hdrs_block.find("helper.cc"), std::string::npos)
      << "a .cc belongs in srcs, not hdrs:\n"
      << build;
}

TEST(GenerateCandidateBuildTest, IncludesAllowedExtraDeps) {
  const std::string build =
      Build({"strategy.h"}, "strategy.h", {"//game_mcts/core/mcts:mcts"});
  EXPECT_NE(build.find("//game_mcts/core/mcts:mcts"), std::string::npos)
      << build;
  // The harness dep is always present and always first.
  EXPECT_NE(build.find("//game_mcts/tournament_server/candidate_api:"
                       "candidate_api"),
            std::string::npos)
      << build;
}

TEST(GenerateCandidateBuildTest, SelectsTheGameByRegistryKey) {
  const std::string build =
      Build({"strategy.h"}, "strategy.h", {}, "tictactoe");
  EXPECT_NE(build.find("CANDIDATE_GAME_TICTACTOE"), std::string::npos) << build;
  EXPECT_EQ(build.find("CANDIDATE_GAME_RISK2"), std::string::npos) << build;
}

// Every one of these is also rejected at submit time; regenerating the check
// here is deliberate, because this function's output is what gets built and it
// must never name a file it was not given.
TEST(GenerateCandidateBuildTest, RefusesUnusableSubmissions) {
  EXPECT_TRUE(Build({}).empty()) << "no files";
  EXPECT_TRUE(Build({"strategy.h"}, "").empty()) << "no entry header";
  EXPECT_TRUE(Build({"strategy.h"}, "other.h").empty())
      << "entry header is not one of the files";
  EXPECT_TRUE(Build({"strategy.h"}, "strategy.h", {}, "chess").empty())
      << "unknown game";
}

TEST(GenerateCandidateBuildTest, IsStableAcrossFileOrder) {
  // A byte-identical BUILD is what keeps a rebuilt candidate's cached actions
  // reusable, so ordering must not leak through.
  EXPECT_EQ(Build({"a.h", "b.h", "c.cc"}, "a.h"),
            Build({"c.cc", "b.h", "a.h"}, "a.h"));
  EXPECT_EQ(Build({"a.h"}, "a.h", {"//x:y", "//p:q"}),
            Build({"a.h"}, "a.h", {"//p:q", "//x:y"}));
}

TEST(CandidateGameDefineTest, MapsKnownGamesOnly) {
  EXPECT_EQ(CandidateGameDefine("risk2"), "CANDIDATE_GAME_RISK2");
  EXPECT_EQ(CandidateGameDefine("tictactoe"), "CANDIDATE_GAME_TICTACTOE");
  EXPECT_TRUE(CandidateGameDefine("chess").empty());
  EXPECT_TRUE(CandidateGameDefine("").empty());
}

}  // namespace
}  // namespace tournament_arena
