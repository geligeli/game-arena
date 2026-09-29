#include "game_arena/server/matchmaking.h"

#include <string>
#include <vector>

#include "gtest/gtest.h"

namespace tournament_arena {
namespace {

using tournament_broker::trueskill::Params;

PoolMember Member(const std::string &id, double mu, double sigma,
                  int running = 0) {
  return {.id = id, .rating = {.mu = mu, .sigma = sigma}, .running = running};
}

std::vector<std::string> Ids(const std::vector<PoolMember> &pool) {
  std::vector<std::string> ids;
  for (const PoolMember &m : pool) {
    ids.push_back(m.id);
  }
  return ids;
}

TEST(PoolOfTest, EveryonePlausiblyInTheTop) {
  // mu - 2 sigma: a 28, b 26, c 25, d 22 (unsure: mu + 2 sigma 30), e 23
  // (sure: mu + 2 sigma 25).
  const std::vector<PoolMember> rated = {Member("c", 27, 1), Member("a", 30, 1),
                                         Member("d", 26, 2), Member("b", 28, 1),
                                         Member("e", 24, 0.5)};
  // The bar is b's 26: c and d could still reach it, e cannot.
  EXPECT_EQ(Ids(PoolOf(rated, 2)),
            (std::vector<std::string>{"a", "b", "c", "d"}));
  EXPECT_EQ(Ids(PoolOf(rated, 10)),
            (std::vector<std::string>{"a", "b", "c", "e", "d"}));
  EXPECT_TRUE(PoolOf({}, 10).empty());
}

TEST(PoolOfTest, OnlyAPlaceNoOutsiderCouldTakeIsSettled) {
  // d could reach 30: a's 28 is not safe from it, b's 26 even less.
  const std::vector<PoolMember> pool =
      PoolOf({Member("a", 30, 1), Member("b", 28, 1), Member("d", 26, 2)}, 2);
  ASSERT_EQ(pool.size(), 3u);
  EXPECT_FALSE(pool[0].settled);
  const std::vector<PoolMember> safe =
      PoolOf({Member("a", 40, 1), Member("b", 28, 1), Member("d", 26, 2)}, 2);
  EXPECT_TRUE(safe[0].settled);
  EXPECT_FALSE(safe[1].settled);
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

TEST(ChoosePairTest, AnOpenPlaceComesBeforeAMoreUncertainSettledOne) {
  std::vector<PoolMember> pool = {Member("settled", 30, 5),
                                  Member("open", 30, 1), Member("near", 29, 1)};
  pool[0].settled = true;
  EXPECT_EQ(ChoosePair(pool, {}, Params{})->first, "open");
}

TEST(ChoosePairTest, BusyMembersYieldToIdleOnes) {
  const std::vector<PoolMember> pool = {Member("a", 30, 4, /*running=*/3),
                                        Member("b", 30, 3), Member("c", 29, 3)};
  EXPECT_EQ(ChoosePair(pool, {}, Params{})->first, "b");
}

TEST(ChoosePairTest, NobodyToPlay) {
  EXPECT_FALSE(ChoosePair({Member("alone", 30, 3)}, {}, Params{}).has_value());
  EXPECT_FALSE(ChoosePair({}, {}, Params{}).has_value());
}

}  // namespace
}  // namespace tournament_arena
