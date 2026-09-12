// Reading the one line a refereed match prints.

#include "game_arena/sandbox/worker/match_tally.h"

#include <string>

#include "gtest/gtest.h"

namespace tournament_arena {
namespace {

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

  ASSERT_TRUE(ParseResultLine(
      "RESULT games=1 wins=0 draws=0 losses=1 elo=-3.5\n", &tally));
  EXPECT_DOUBLE_EQ(tally.elo, -3.5);
}

TEST(ParseResultLineTest, LastResultWins) {
  RunTally tally;
  ASSERT_TRUE(
      ParseResultLine("RESULT games=1 wins=1 draws=0 losses=0 elo=1500.0\n"
                      "RESULT games=2 wins=0 draws=0 losses=2 elo=1470.0\n",
                      &tally));
  EXPECT_EQ(tally.games, 2);
  EXPECT_DOUBLE_EQ(tally.elo, 1470.0);
}

}  // namespace
}  // namespace tournament_arena
