// The generated BUILD is the one piece of the pipeline an agent never sees and
// cannot debug, so it is asserted directly rather than only through whether a
// build happened to succeed.

#include "game_mcts/tournament_server/sandbox_worker/candidate_build.h"

#include <string>

#include "gtest/gtest.h"

namespace tournament_arena {
namespace {

auto MakeOrder() -> proto::WorkOrder {
  proto::WorkOrder order;
  order.set_order_id("o1");
  order.set_candidate_id("my-bot-abc123");
  order.set_game("risk2");
  order.set_entry_header("strategy.h");
  auto *file = order.add_files();
  file->set_path("strategy.h");
  file->set_content("// header\n");
  return order;
}

TEST(CandidateTargetTest, LabelsTheGeneratedBinary) {
  EXPECT_EQ(CandidateTarget("my-bot-abc123"),
            "//game_mcts/tournament_server/candidates/my-bot-abc123:bot");
}

TEST(GenerateCandidateBuildTest, WiresTheEntryHeaderAndGameIntoTheBinary) {
  const std::string build = GenerateCandidateBuild(MakeOrder());
  ASSERT_FALSE(build.empty());

  EXPECT_NE(build.find("name = \"strategy\""), std::string::npos);
  EXPECT_NE(build.find("name = \"bot\""), std::string::npos);
  EXPECT_NE(build.find("\"strategy.h\","), std::string::npos);
  // The harness main is compiled here, not depended on, because the defines
  // below would not reach a prebuilt library.
  EXPECT_NE(build.find("//game_mcts/tournament_server/candidate:candidate_main.cc"),
            std::string::npos);
  EXPECT_NE(
      build.find(
          "CANDIDATE_ENTRY_HEADER=\\\"game_mcts/tournament_server/candidates/"
          "my-bot-abc123/strategy.h\\\""),
      std::string::npos);
  EXPECT_NE(build.find("CANDIDATE_GAME_RISK2"), std::string::npos);
  EXPECT_EQ(build.find("CANDIDATE_GAME_TICTACTOE"), std::string::npos);
  // Private: nothing else in the clone should be able to depend on a
  // submission.
  EXPECT_NE(build.find("//visibility:private"), std::string::npos);
}

TEST(GenerateCandidateBuildTest, SeparatesHeadersFromCompiledSources) {
  proto::WorkOrder order = MakeOrder();
  order.add_files()->set_path("helper.cc");
  order.add_files()->set_path("tables.inl");
  order.add_files()->set_path("extra.hpp");

  const std::string build = GenerateCandidateBuild(order);
  const auto hdrs = build.find("hdrs = [");
  const auto srcs = build.find("srcs = [\n        \"helper.cc\"");
  ASSERT_NE(hdrs, std::string::npos);
  ASSERT_NE(srcs, std::string::npos);

  // .inl and .hpp are headers; only .cc/.cpp get compiled on their own.
  const std::string hdr_block = build.substr(hdrs, srcs - hdrs);
  EXPECT_NE(hdr_block.find("tables.inl"), std::string::npos);
  EXPECT_NE(hdr_block.find("extra.hpp"), std::string::npos);
  EXPECT_EQ(hdr_block.find("helper.cc"), std::string::npos);
}

TEST(GenerateCandidateBuildTest, IncludesAllowedExtraDeps) {
  proto::WorkOrder order = MakeOrder();
  order.add_extra_deps("//game_mcts/cpp/risk/strategies:risk_proposer");
  order.add_extra_deps("//game_mcts/cpp/mcts:mcts");

  const std::string build = GenerateCandidateBuild(order);
  EXPECT_NE(build.find("//game_mcts/cpp/risk/strategies:risk_proposer"),
            std::string::npos);
  EXPECT_NE(build.find("//game_mcts/cpp/mcts:mcts"),
            std::string::npos);
  EXPECT_NE(build.find("//game_mcts/tournament_server/candidate:candidate_api"),
            std::string::npos);
}

TEST(GenerateCandidateBuildTest, SelectsTheGameByRegistryKey) {
  proto::WorkOrder order = MakeOrder();
  order.set_game("tictactoe");
  const std::string build = GenerateCandidateBuild(order);
  EXPECT_NE(build.find("CANDIDATE_GAME_TICTACTOE"), std::string::npos);
  EXPECT_EQ(build.find("CANDIDATE_GAME_RISK2"), std::string::npos);
}

// Refusing to generate is how the worker avoids writing a BUILD that names a
// file it never received, which would fail as a confusing compile error.
TEST(GenerateCandidateBuildTest, RefusesUnusableOrders) {
  proto::WorkOrder no_files = MakeOrder();
  no_files.clear_files();
  EXPECT_TRUE(GenerateCandidateBuild(no_files).empty());

  proto::WorkOrder no_entry = MakeOrder();
  no_entry.clear_entry_header();
  EXPECT_TRUE(GenerateCandidateBuild(no_entry).empty());

  proto::WorkOrder wrong_entry = MakeOrder();
  wrong_entry.set_entry_header("not_submitted.h");
  EXPECT_TRUE(GenerateCandidateBuild(wrong_entry).empty());

  proto::WorkOrder unknown_game = MakeOrder();
  unknown_game.set_game("chess");
  EXPECT_TRUE(GenerateCandidateBuild(unknown_game).empty());
}

TEST(GenerateCandidateBuildTest, IsStableAcrossFileOrder) {
  proto::WorkOrder first = MakeOrder();
  first.add_files()->set_path("a.cc");
  first.add_files()->set_path("z.cc");

  proto::WorkOrder second = MakeOrder();
  second.add_files()->set_path("z.cc");
  second.add_files()->set_path("a.cc");

  // A byte-identical BUILD is what lets a rebuilt candidate hit the disk cache.
  EXPECT_EQ(GenerateCandidateBuild(first), GenerateCandidateBuild(second));
}

TEST(FormatParamsTest, IsSortedSoRebuildsMatch) {
  proto::WorkOrder order = MakeOrder();
  (*order.mutable_params())["iterations"] = "800";
  (*order.mutable_params())["exploration_c"] = "1.4";
  EXPECT_EQ(FormatParams(order), "exploration_c=1.4,iterations=800");

  proto::WorkOrder empty = MakeOrder();
  EXPECT_EQ(FormatParams(empty), "");
}

TEST(CandidateGameDefineTest, MapsKnownGamesOnly) {
  EXPECT_EQ(CandidateGameDefine("risk2"), "CANDIDATE_GAME_RISK2");
  EXPECT_EQ(CandidateGameDefine("tictactoe"), "CANDIDATE_GAME_TICTACTOE");
  EXPECT_TRUE(CandidateGameDefine("chess").empty());
  EXPECT_TRUE(CandidateGameDefine("").empty());
}

}  // namespace
}  // namespace tournament_arena
