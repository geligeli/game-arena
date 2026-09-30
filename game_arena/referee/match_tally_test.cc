#include "game_arena/referee/match_tally.h"

#include <gtest/gtest.h>

#include <vector>

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

TEST(MatchTallyTest, CountsPlacesOfAFreeForAll) {
  proto::MatchReport report;
  for (const std::vector<int> &places :
       {std::vector{0, 1, 2}, std::vector{2, 0, 1}, std::vector{1, 2, 0}}) {
    proto::GameRecord *record = report.add_games();
    for (const char *name : {"me", "you", "them"}) {
      record->add_player_names(name);
    }
    *record->mutable_places() = {places.begin(), places.end()};
  }
  const MatchTally tally = TallyOf(report, "me");
  EXPECT_EQ(tally.games, 3);
  EXPECT_EQ(tally.finishes, (std::vector{1, 1, 1}));
  EXPECT_EQ(tally.wins, 1);
  EXPECT_EQ(tally.losses, 2);
  EXPECT_EQ(
      RecordText(3, tally.wins, tally.draws, tally.losses, tally.finishes),
      "1st/2nd/3rd 1/1/1");
  EXPECT_EQ(RecordText(2, 4, 1, 3, {}), "W/D/L 4/1/3");
  EXPECT_EQ(Ordinal(10), "11th");
  EXPECT_EQ(Ordinal(21), "22nd");
}

TEST(MatchTallyTest, SkipsAGameThePlayerWasNotIn) {
  MatchTally tally;
  EXPECT_FALSE(
      AddGame(Game("a", "b", proto::GameRecord::WIN, 0), "me", &tally));
  EXPECT_EQ(tally.games, 0);
}

}  // namespace
}  // namespace tournament_broker
