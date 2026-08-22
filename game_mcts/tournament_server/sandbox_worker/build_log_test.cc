// Log compaction is what stands between an agent and a megabyte of bazel
// output, so the thing worth testing is that the diagnostic lines survive and
// the noise does not.

#include "cpp/tournament_server/sandbox_worker/build_log.h"

#include <string>

#include "gtest/gtest.h"

namespace tournament_arena {
namespace {

TEST(CompactBuildLogTest, KeepsCompilerErrorsAndDropsProgressNoise) {
  const std::string log = R"(
INFO: Analyzed target //game_mcts/tournament_server/candidates/bot-abc123:bot
[1,234 / 5,678] Compiling cpp/risk/risk_game.cpp; 3s processwrapper-sandbox
[2,345 / 5,678] Compiling cpp/mcts/mcts.cpp; 1s processwrapper-sandbox
In file included from cpp/tournament_server/candidates/bot-abc123/strategy.h:5:
cpp/tournament_server/candidates/bot-abc123/strategy.h:22:10: error: 'foo' was not declared in this scope
   22 |   return foo(state);
      |          ^~~
[3,456 / 5,678] Compiling cpp/risk/risk_board.cpp; 2s
INFO: Elapsed time: 41.2s, Critical Path: 12.3s
ERROR: Build did NOT complete successfully
)";

  const std::string compact = CompactBuildLog(log);
  EXPECT_NE(compact.find("error: 'foo' was not declared"), std::string::npos);
  EXPECT_NE(compact.find("strategy.h:22:10"), std::string::npos);
  EXPECT_NE(compact.find("In file included from"), std::string::npos);
  EXPECT_NE(compact.find("ERROR: Build did NOT complete"), std::string::npos);

  // The progress spam is the bulk of a real log and none of the signal.
  EXPECT_EQ(compact.find("Compiling cpp/risk/risk_game.cpp"),
            std::string::npos);
  EXPECT_EQ(compact.find("Analyzed target"), std::string::npos);
  EXPECT_EQ(compact.find("Elapsed time"), std::string::npos);
  EXPECT_LT(compact.size(), log.size());
}

TEST(CompactBuildLogTest, FallsBackToTheTailWhenNothingIsRecognised) {
  const std::string log =
      "something went wrong in a way we do not have a pattern for\n"
      "and here is the interesting part at the end\n";
  const std::string compact = CompactBuildLog(log);
  // An unrecognised failure still has to be diagnosable.
  EXPECT_NE(compact.find("interesting part at the end"), std::string::npos);
}

TEST(CompactBuildLogTest, IsBoundedEvenForAPathologicalLog) {
  std::string log;
  for (int i = 0; i < 5000; ++i) {
    log += "strategy.h:" + std::to_string(i) + ":1: error: too many errors\n";
  }
  BuildLogLimits limits;
  limits.max_chars = 2000;
  limits.max_lines = 20;
  const std::string compact = CompactBuildLog(log, limits);
  EXPECT_LE(compact.size(), 2100u);  // plus the truncation marker
  EXPECT_NE(compact.find("further diagnostics omitted"), std::string::npos);
}

TEST(TailOfTest, MarksWhatItDropped) {
  EXPECT_EQ(TailOf("short", 100), "short");
  const std::string tail = TailOf(std::string(1000, 'x'), 100);
  EXPECT_NE(tail.find("900 chars truncated"), std::string::npos);
  EXPECT_LT(tail.size(), 200u);
}

TEST(ParseResultLineTest, ReadsTheHarnessSummary) {
  RunTally tally;
  ASSERT_TRUE(ParseResultLine(
      "some noise\nRESULT games=5 wins=3 draws=1 losses=1 elo=1512.4\n",
      &tally));
  EXPECT_EQ(tally.games, 5);
  EXPECT_EQ(tally.wins, 3);
  EXPECT_EQ(tally.draws, 1);
  EXPECT_EQ(tally.losses, 1);
  EXPECT_DOUBLE_EQ(tally.elo, 1512.4);
}

TEST(ParseResultLineTest, HandlesNegativeAndMissingResults) {
  RunTally tally;
  EXPECT_FALSE(ParseResultLine("bot crashed\n", &tally));
  EXPECT_FALSE(ParseResultLine("", &tally));
  // A partial line is not a result.
  EXPECT_FALSE(ParseResultLine("RESULT games=5 wins=3\n", &tally));

  ASSERT_TRUE(
      ParseResultLine("RESULT games=1 wins=0 draws=0 losses=1 elo=-3.5\n",
                      &tally));
  EXPECT_DOUBLE_EQ(tally.elo, -3.5);
}

TEST(ParseResultLineTest, LastResultWins) {
  RunTally tally;
  ASSERT_TRUE(ParseResultLine(
      "RESULT games=1 wins=1 draws=0 losses=0 elo=1500.0\n"
      "RESULT games=2 wins=0 draws=0 losses=2 elo=1470.0\n",
      &tally));
  EXPECT_EQ(tally.games, 2);
  EXPECT_DOUBLE_EQ(tally.elo, 1470.0);
}

}  // namespace
}  // namespace tournament_arena
