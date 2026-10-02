#include "game_arena/server/matchmaking.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "gtest/gtest.h"

namespace tournament_arena {
namespace {

using tournament_broker::trueskill::Params;

constexpr double kBeta = Params{}.beta;

PoolMember Member(const std::string &id, double mu, double sigma,
                  int running = 0, const std::string &author = "") {
  return {.id = id,
          .author = author.empty() ? id : author,
          .rating = {.mu = mu, .sigma = sigma},
          .running = running};
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
  EXPECT_EQ(Ids(PoolOf(rated, 2, 2, kBeta)),
            (std::vector<std::string>{"a", "b", "c", "d"}));
  EXPECT_EQ(Ids(PoolOf(rated, 10, 2, kBeta)),
            (std::vector<std::string>{"a", "b", "c", "e", "d"}));
  EXPECT_TRUE(PoolOf({}, 10, 2, kBeta).empty());
}

TEST(PoolOfTest, OnlyAPlaceNoOutsiderCouldTakeIsSettled) {
  // d could reach 30: a's 28 is not safe from it, b's 26 even less.
  const std::vector<PoolMember> pool =
      PoolOf({Member("a", 30, 1), Member("b", 28, 1), Member("d", 26, 2)}, 2, 2, kBeta);
  ASSERT_EQ(pool.size(), 3u);
  EXPECT_FALSE(pool[0].settled);
  const std::vector<PoolMember> safe =
      PoolOf({Member("a", 40, 1), Member("b", 28, 1), Member("d", 26, 2)}, 2, 2, kBeta);
  EXPECT_TRUE(safe[0].settled);
  EXPECT_FALSE(safe[1].settled);
}

// opus rated against x and y alone, then z arrived: z sat in 10 of its 100
// games where a fair share, two seats over three other authors, is 2/3.
TEST(PoolOfTest, ALopsidedFieldIsDoubtAndUnsettlesAPlace) {
  PoolMember opus = Member("opus", 36, 0.2, 0, "opus");
  opus.games = 100;
  opus.met = {{"x", 100}, {"y", 90}, {"z", 10}};
  // x2 is outside the top 4 but could reach 32.
  const std::vector<PoolMember> rated = {opus, Member("x", 30, 0.2, 0, "x"),
                                         Member("y", 29, 0.2, 0, "y"),
                                         Member("z", 28, 0.2, 0, "z"),
                                         Member("x2", 28, 2, 0, "x")};
  const std::vector<PoolMember> pool = PoolOf(rated, 4, 3, kBeta);
  ASSERT_EQ(pool.front().id, "opus");
  // x and y it met its share of.
  EXPECT_EQ(pool.front().shortfall.size(), 1u);
  EXPECT_NEAR(pool.front().shortfall.at("z"), 2.0 / 3 - 0.1, 1e-9);
  EXPECT_NEAR(Doubt(pool.front(), kBeta),
              std::hypot(0.2, kBeta * (2.0 / 3 - 0.1)), 1e-9);
  // 36 - 2 * 0.2 is safe from x2's 32, 36 - 2 * 2.4 is not.
  EXPECT_FALSE(pool.front().settled);
  opus.met["z"] = 70;
  EXPECT_TRUE(PoolOf({opus, rated[1], rated[2], rated[3], rated[4]}, 4, 3,
                     kBeta)
                  .front()
                  .shortfall.empty());
  // A version yet to play is short of no one.
  EXPECT_TRUE(PoolOf({Member("new", 25, 8, 0, "new"), rated[1]}, 4, 3, kBeta)
                  .front()
                  .shortfall.empty());
}

TEST(ChooseGroupTest, TheLeastCertainMeetsItsClosestUncertainRival) {
  const std::vector<PoolMember> pool = {
      Member("sure", 30, 1), Member("unsure", 30, 5), Member("near", 29, 3),
      Member("far", 10, 3)};
  const auto group = ChooseGroup(pool, {}, Params{}, 2);
  ASSERT_TRUE(group.has_value());
  EXPECT_EQ(*group, (std::vector<std::string>{"unsure", "near"}));
}

TEST(ChooseGroupTest, RecentOpponentsWaitWhileAnotherIsLeft) {
  const std::vector<PoolMember> pool = {
      Member("unsure", 30, 5), Member("near", 29, 3), Member("next", 27, 3)};
  std::map<std::string, std::deque<std::string>> recent = {
      {"unsure", {"near"}}};
  EXPECT_EQ(ChooseGroup(pool, recent, Params{}, 2)->at(1), "next");
  recent["unsure"] = {"near", "next"};
  // Everyone was met lately: the best of them after all.
  EXPECT_EQ(ChooseGroup(pool, recent, Params{}, 2)->at(1), "near");
}

TEST(ChooseGroupTest, AnOpenPlaceComesBeforeAMoreUncertainSettledOne) {
  std::vector<PoolMember> pool = {Member("settled", 30, 5),
                                  Member("open", 30, 1), Member("near", 29, 1)};
  pool[0].settled = true;
  EXPECT_EQ(ChooseGroup(pool, {}, Params{}, 2)->front(), "open");
}

TEST(ChooseGroupTest, BusyMembersYieldToIdleOnes) {
  const std::vector<PoolMember> pool = {Member("a", 30, 4, /*running=*/3),
                                        Member("b", 30, 3), Member("c", 29, 3)};
  EXPECT_EQ(ChooseGroup(pool, {}, Params{}, 2)->front(), "b");
}

TEST(ChooseGroupTest, ThreeSeatsTakeTheBestPairOfRivals) {
  const std::vector<PoolMember> pool = {
      Member("unsure", 30, 5), Member("near", 29, 3), Member("next", 27, 3),
      Member("far", 10, 3)};
  EXPECT_EQ(*ChooseGroup(pool, {}, Params{}, 3),
            (std::vector<std::string>{"unsure", "near", "next"}));
}

// Two versions of one author could gang up on a third seat, so a group of
// three authors wins over a closer one; two seats have no one to gang up on.
TEST(ChooseGroupTest, ThreeSeatsPreferDistinctAuthors) {
  const std::vector<PoolMember> pool = {
      Member("unsure", 30, 5, 0, "x"), Member("near", 29, 3, 0, "x"),
      Member("next", 27, 3, 0, "y"), Member("third", 25, 3, 0, "z")};
  EXPECT_EQ(*ChooseGroup(pool, {}, Params{}, 3),
            (std::vector<std::string>{"unsure", "next", "third"}));
  EXPECT_EQ(ChooseGroup(pool, {}, Params{}, 2)->at(1), "near");
  // Only one other author left: the best group after all.
  EXPECT_EQ(*ChooseGroup({pool[0], pool[1], pool[2]}, {}, Params{}, 3),
            (std::vector<std::string>{"unsure", "near", "next"}));
}

// A settled leader short of z's games is the most doubtful member, and meets
// z, however far, before closer rivals it has met enough of.
TEST(ChooseGroupTest, AMemberMeetsTheAuthorsItIsShortestOf) {
  PoolMember leader = Member("leader", 36, 0.2, 0, "opus");
  leader.shortfall = {{"z", 0.6}};
  const std::vector<PoolMember> pool = {
      leader, Member("x1", 35, 1, 0, "x"), Member("y1", 34, 1, 0, "y"),
      Member("z1", 25, 1, 0, "z")};
  const auto group = ChooseGroup(pool, {}, Params{}, 3);
  EXPECT_EQ(group->front(), "leader");
  EXPECT_TRUE(std::ranges::contains(*group, std::string("z1")));
  // Short of no one, it has the least doubt of all.
  leader.shortfall.clear();
  EXPECT_EQ(ChooseGroup({leader, pool[1], pool[2], pool[3]}, {}, Params{}, 3)
                ->front(),
            "x1");
}

// Two versions cannot fill three seats alone: both play, and the builtin
// that makes the closest game takes the third.
TEST(ChooseGroupTest, BuiltinsFillAPoolTooSmallForTheSeats) {
  const std::vector<PoolMember> pool = {Member("a", 25, 3), Member("b", 24, 3)};
  const std::vector<PoolMember> fillers = {Member("builtin:far", 5, 1),
                                           Member("builtin:near", 24, 1)};
  EXPECT_EQ(*ChooseGroup(pool, {}, Params{}, 3, fillers),
            (std::vector<std::string>{"a", "b", "builtin:near"}));
  // One version, or a pool big enough, plays no builtin.
  EXPECT_FALSE(ChooseGroup({pool[0]}, {}, Params{}, 3, fillers).has_value());
  const std::vector<PoolMember> three = {pool[0], pool[1], Member("c", 23, 3)};
  EXPECT_EQ(ChooseGroup(three, {}, Params{}, 3, fillers)->size(), 3u);
  EXPECT_FALSE(
      std::ranges::contains(*ChooseGroup(three, {}, Params{}, 3, fillers),
                            std::string("builtin:near")));
}

TEST(ChooseGroupTest, NobodyToPlay) {
  EXPECT_FALSE(
      ChooseGroup({Member("alone", 30, 3)}, {}, Params{}, 2).has_value());
  EXPECT_FALSE(ChooseGroup({}, {}, Params{}, 2).has_value());
  EXPECT_FALSE(
      ChooseGroup({Member("a", 30, 3), Member("b", 30, 3)}, {}, Params{}, 3)
          .has_value());
}

}  // namespace
}  // namespace tournament_arena
