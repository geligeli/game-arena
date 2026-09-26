// Reading the one line a refereed match prints.

#include "game_arena/sandbox/worker/match_tally.h"

#include <string>

#include "gtest/gtest.h"

namespace tournament_arena {
namespace {

TEST(ParseResultLineTest, ReadsTheHarnessSummary) {
  RunTally tally;
  ASSERT_TRUE(ParseResultLine(
      "some noise\nRESULT games=5 wins=3 draws=1 losses=1\n", &tally));
  EXPECT_EQ(tally.games, 5);
  EXPECT_EQ(tally.wins, 3);
  EXPECT_EQ(tally.draws, 1);
  EXPECT_EQ(tally.losses, 1);
}

TEST(ParseResultLineTest, HandlesMissingResults) {
  RunTally tally;
  EXPECT_FALSE(ParseResultLine("bot crashed\n", &tally));
  EXPECT_FALSE(ParseResultLine("", &tally));
  // A partial line is not a result.
  EXPECT_FALSE(ParseResultLine("RESULT games=5 wins=3\n", &tally));

  ASSERT_TRUE(
      ParseResultLine("RESULT games=1 wins=0 draws=0 losses=1\n", &tally));
}

TEST(ParseResultLineTest, LastResultWins) {
  RunTally tally;
  ASSERT_TRUE(
      ParseResultLine("RESULT games=1 wins=1 draws=0 losses=0\n"
                      "RESULT games=2 wins=0 draws=0 losses=2\n",
                      &tally));
  EXPECT_EQ(tally.games, 2);
}

}  // namespace
}  // namespace tournament_arena
