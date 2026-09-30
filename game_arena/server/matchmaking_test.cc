#include "game_arena/server/matchmaking.h"

#include <string>
#include <vector>

#include "gtest/gtest.h"

namespace tournament_arena {
namespace {

using tournament_broker::trueskill::Params;

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
