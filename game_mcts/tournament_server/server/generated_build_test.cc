#include "game_mcts/tournament_server/server/generated_build.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace tournament_arena {
namespace {

// Deliberately not this repo's labels. The coordinator generates a BUILD out
// of whatever the problem config names; if these were game_mcts paths, a
// regression that reintroduced a hardcoded one would still pass.
constexpr char kDir[] = "solutions";
constexpr char kApiDep[] = "//problem/harness:api";
constexpr char kMainSrc[] = "//problem/harness:main.cc";

auto Harness(const std::string &game_define = "PROBLEM_GAME_ALPHA")
    -> proto::CandidateHarness {
  proto::CandidateHarness harness;
  harness.set_api_dep(kApiDep);
  harness.set_main_src(kMainSrc);
  harness.add_bot_deps("//problem/harness:client");
  harness.set_game_define(game_define);
  return harness;
}

auto Build(const std::vector<std::string> &files,
           const std::string &entry = "strategy.h",
           const std::vector<std::string> &deps = {},
           const proto::CandidateHarness &harness = Harness()) -> std::string {
  return GenerateCandidateBuild(kDir, "c-1", harness, files, entry, deps);
}

TEST(CandidateTargetTest, LabelsTheGeneratedBinary) {
  EXPECT_EQ(CandidateTarget(kDir, "my-bot-abc123"),
            "//solutions/my-bot-abc123:bot");
}

TEST(GenerateCandidateBuildTest, WiresTheEntryHeaderAndGameIntoTheBinary) {
  const std::string build = Build({"strategy.h"});
  EXPECT_NE(build.find("name = \"strategy\""), std::string::npos) << build;
  EXPECT_NE(build.find("name = \"bot\""), std::string::npos) << build;
  // The harness reaches the submission through this define, not a dep: the game
  // and the entry header are local_defines, which do not reach a prebuilt lib.
  EXPECT_NE(
      build.find("CANDIDATE_ENTRY_HEADER=\\\"solutions/c-1/strategy.h\\\""),
      std::string::npos)
      << build;
  EXPECT_NE(build.find("PROBLEM_GAME_ALPHA"), std::string::npos) << build;
  EXPECT_NE(build.find(kMainSrc), std::string::npos) << build;
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
      Build({"strategy.h"}, "strategy.h", {"//problem/lib:extra"});
  EXPECT_NE(build.find("//problem/lib:extra"), std::string::npos) << build;
  // The harness dep is always present and always first.
  EXPECT_NE(build.find(kApiDep), std::string::npos) << build;
}

// The whole point of the harness config: which library a solution links is the
// problem's business, and two problems may answer differently.
TEST(GenerateCandidateBuildTest, TakesEveryLabelFromTheProblemConfig) {
  proto::CandidateHarness other;
  other.set_api_dep("//other/problem:sdk");
  other.set_main_src("//other/problem:entry.cc");
  other.add_bot_deps("//other/problem:runtime");
  other.set_game_define("OTHER_GAME");

  const std::string build = Build({"strategy.h"}, "strategy.h", {}, other);
  EXPECT_NE(build.find("//other/problem:sdk"), std::string::npos) << build;
  EXPECT_NE(build.find("//other/problem:entry.cc"), std::string::npos) << build;
  EXPECT_NE(build.find("//other/problem:runtime"), std::string::npos) << build;
  EXPECT_NE(build.find("OTHER_GAME"), std::string::npos) << build;
  EXPECT_EQ(build.find(kApiDep), std::string::npos)
      << "no label may survive from anywhere but the config:\n"
      << build;
}

// A harness serving a single game needs no selection define, and emitting an
// empty one would be a build error rather than a no-op.
TEST(GenerateCandidateBuildTest, OmitsTheGameDefineWhenUnset) {
  const std::string build =
      Build({"strategy.h"}, "strategy.h", {}, Harness(/*game_define=*/""));
  ASSERT_FALSE(build.empty());
  EXPECT_EQ(build.find("local_defines = [\"\"]"), std::string::npos) << build;
  EXPECT_NE(build.find("CANDIDATE_ENTRY_HEADER"), std::string::npos) << build;
}

// Every one of these is also rejected at submit time; regenerating the check
// here is deliberate, because this function's output is what gets built and it
// must never name a file it was not given.
TEST(GenerateCandidateBuildTest, RefusesUnusableSubmissions) {
  EXPECT_TRUE(Build({}).empty()) << "no files";
  EXPECT_TRUE(Build({"strategy.h"}, "").empty()) << "no entry header";
  EXPECT_TRUE(Build({"strategy.h"}, "other.h").empty())
      << "entry header is not one of the files";
  EXPECT_TRUE(Build({"strategy.h"}, "strategy.h", {}, proto::CandidateHarness{})
                  .empty())
      << "a problem with no harness configured has no structured form";
}

TEST(GenerateCandidateBuildTest, IsStableAcrossFileOrder) {
  // A byte-identical BUILD is what keeps a rebuilt candidate's cached actions
  // reusable, so ordering must not leak through.
  EXPECT_EQ(Build({"a.h", "b.h", "c.cc"}, "a.h"),
            Build({"c.cc", "b.h", "a.h"}, "a.h"));
  EXPECT_EQ(Build({"a.h"}, "a.h", {"//x:y", "//p:q"}),
            Build({"a.h"}, "a.h", {"//p:q", "//x:y"}));
}

}  // namespace
}  // namespace tournament_arena
