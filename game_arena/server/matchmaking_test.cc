#include "game_arena/server/matchmaking.h"

#include <string>
#include <vector>

#include "gtest/gtest.h"

namespace tournament_arena {
namespace {

using tournament_broker::trueskill::Params;

PoolMember Member(const std::string &id, double mu, double sigma,
                  int pool_games = 100, int running = 0) {
  return {.id = id,
          .rating = {.mu = mu, .sigma = sigma},
          .pool_games = pool_games,
          .running = running};
}

std::vector<std::string> Ids(const std::vector<PoolMember> &pool) {
  std::vector<std::string> ids;
  for (const PoolMember &m : pool) {
    ids.push_back(m.id);
  }
  return ids;
}

TEST(PoolOfTest, TheTopByConservativeScorePlusNewcomers) {
  // Conservative scores: a 27, b 24, c 21, d 12 (a newcomer), e 18.
  const std::vector<PoolMember> rated = {
      Member("c", 30, 3), Member("a", 30, 1), Member("d", 30, 6, 10),
      Member("b", 27, 1), Member("e", 21, 1)};
  EXPECT_EQ(Ids(PoolOf(rated, 2, 40)),
            (std::vector<std::string>{"a", "b", "d"}));
  EXPECT_EQ(Ids(PoolOf(rated, 10, 40)),
            (std::vector<std::string>{"a", "b", "c", "e", "d"}));
}

TEST(ChoosePairTest, TheLeastCertainMeetsItsClosestUncertainRival) {
  const std::vector<PoolMember> pool = {
      Member("sure", 30, 1), Member("unsure", 30, 5), Member("near", 29, 3),
      Member("far", 10, 3)};
  const auto pair = ChoosePair(pool, {}, Params{});
  ASSERT_TRUE(pair.has_value());
  EXPECT_EQ(pair->first, "unsure");
  EXPECT_EQ(pair->second, "near");
}

TEST(ChoosePairTest, RecentOpponentsWaitWhileAnotherIsLeft) {
  const std::vector<PoolMember> pool = {
      Member("unsure", 30, 5), Member("near", 29, 3), Member("next", 27, 3)};
  std::map<std::string, std::deque<std::string>> recent = {
      {"unsure", {"near"}}};
  EXPECT_EQ(ChoosePair(pool, recent, Params{})->second, "next");
  recent["unsure"] = {"near", "next"};
  // Everyone was met lately: the best of them after all.
  EXPECT_EQ(ChoosePair(pool, recent, Params{})->second, "near");
}

TEST(ChoosePairTest, BusyMembersYieldToIdleOnes) {
  const std::vector<PoolMember> pool = {Member("a", 30, 4, 100, /*running=*/3),
                                        Member("b", 30, 3), Member("c", 29, 3)};
  EXPECT_EQ(ChoosePair(pool, {}, Params{})->first, "b");
}

TEST(ChoosePairTest, NobodyToPlay) {
  EXPECT_FALSE(ChoosePair({Member("alone", 30, 3)}, {}, Params{}).has_value());
  EXPECT_FALSE(ChoosePair({}, {}, Params{}).has_value());
}

}  // namespace
}  // namespace tournament_arena
