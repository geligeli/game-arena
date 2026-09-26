#include "game_arena/referee/match_tally.h"

#include <gtest/gtest.h>

namespace tournament_broker {
namespace {

proto::GameRecord Game(const std::string &a, const std::string &b,
                       proto::GameRecord::Result result, int winner) {
  proto::GameRecord record;
  record.add_player_names(a);
  record.add_player_names(b);
  record.set_result(result);
  record.set_winning_player(winner);
  return record;
}

TEST(MatchTallyTest, CountsFromThePlayersSeatWhicheverItIs) {
  proto::MatchReport report;
  *report.add_games() = Game("me", "you", proto::GameRecord::WIN, 0);
  *report.add_games() = Game("you", "me", proto::GameRecord::WIN, 1);
  *report.add_games() = Game("you", "me", proto::GameRecord::WIN, 0);
  *report.add_games() = Game("me", "you", proto::GameRecord::DRAW, 0);
  const MatchTally tally = TallyOf(report, "me");
  EXPECT_EQ(tally.games, 4);
  EXPECT_EQ(tally.wins, 2);
  EXPECT_EQ(tally.draws, 1);
  EXPECT_EQ(tally.losses, 1);
}

TEST(MatchTallyTest, SkipsAGameThePlayerWasNotIn) {
  MatchTally tally;
  EXPECT_FALSE(
      AddGame(Game("a", "b", proto::GameRecord::WIN, 0), "me", &tally));
  EXPECT_EQ(tally.games, 0);
}

}  // namespace
}  // namespace tournament_broker
