#include "game_arena/standings/trueskill.h"

#include <cmath>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "gtest/gtest.h"

namespace tournament_broker::trueskill {
namespace {

// Reference values: the Python `trueskill` package with its default
// environment (the same defaults as Params).
TEST(Rate1v1Test, FreshPlayersWinMatchesReference) {
  const Params params;
  const auto [winner, loser] =
      Rate1v1(params.Initial(), params.Initial(), /*draw=*/false, params);
  EXPECT_NEAR(winner.mu, 29.396, 1e-3);
  EXPECT_NEAR(winner.sigma, 7.171, 1e-3);
  EXPECT_NEAR(loser.mu, 20.604, 1e-3);
  EXPECT_NEAR(loser.sigma, 7.171, 1e-3);
}

TEST(Rate1v1Test, FreshPlayersDrawMatchesReference) {
  const Params params;
  const auto [a, b] =
      Rate1v1(params.Initial(), params.Initial(), /*draw=*/true, params);
  EXPECT_NEAR(a.mu, 25.0, 1e-3);
  EXPECT_NEAR(a.sigma, 6.458, 1e-3);
  EXPECT_NEAR(b.mu, 25.0, 1e-3);
  EXPECT_NEAR(b.sigma, 6.458, 1e-3);
}

TEST(DrawMarginTest, MatchesInverseCdf) {
  Params params;
  // Phi^-1(0.55) * sqrt(2) * 25/6.
  EXPECT_NEAR(DrawMargin(params), 0.740466, 1e-6);
  params.draw_probability = 0.0;
  EXPECT_NEAR(DrawMargin(params), 0.0, 1e-12);
}

TEST(Rate1v1Test, UpsetMovesRatingsMoreThanExpectedWin) {
  const Params params;
  const Rating strong{30.0, 3.0};
  const Rating weak{20.0, 3.0};
  const auto [expected_w, expected_l] =
      Rate1v1(strong, weak, /*draw=*/false, params);
  const auto [upset_w, upset_l] = Rate1v1(weak, strong, /*draw=*/false, params);
  EXPECT_GT(upset_w.mu - weak.mu, expected_w.mu - strong.mu);
  EXPECT_GT(strong.mu - upset_l.mu, weak.mu - expected_l.mu);
  // Equal sigmas: the winner gains exactly what the loser drops.
  EXPECT_NEAR(upset_w.mu - weak.mu, strong.mu - upset_l.mu, 1e-12);
}

TEST(Rate1v1Test, ExtremeMismatchStaysFinite) {
  const Params params;
  const Rating giant{200.0, 1.0};
  const Rating minnow{0.0, 1.0};
  const std::pair<Rating, Rating> games[] = {{giant, minnow}, {minnow, giant}};
  for (const bool draw : {false, true}) {
    for (const auto &[a, b] : games) {
      const auto [ra, rb] = Rate1v1(a, b, draw, params);
      for (const Rating &r : {ra, rb}) {
        EXPECT_TRUE(std::isfinite(r.mu));
        EXPECT_TRUE(std::isfinite(r.sigma));
        EXPECT_GT(r.sigma, 0.0);
      }
    }
  }
  // The expected result teaches next to nothing.
  const auto [w, l] = Rate1v1(giant, minnow, /*draw=*/false, params);
  EXPECT_NEAR(w.mu, giant.mu, 1e-6);
  EXPECT_NEAR(l.mu, minnow.mu, 1e-6);
}

TEST(Rate1v1Test, DrawWithoutDrawProbabilityPullsTogether) {
  Params params;
  params.draw_probability = 0.0;
  const auto [a, b] =
      Rate1v1(Rating{30.0, 4.0}, Rating{20.0, 4.0}, /*draw=*/true, params);
  EXPECT_LT(a.mu, 30.0);
  EXPECT_GT(b.mu, 20.0);
  EXPECT_LT(a.sigma, 4.0);
}

// The message passing agrees with the closed form where both apply.
TEST(RateFreeForAllTest, TwoPlayersMatchRate1v1) {
  for (const double draw_probability : {0.0, 0.1}) {
    Params params;
    params.draw_probability = draw_probability;
    const std::vector<Rating> ratings = {{30.0, 4.0}, {20.0, 5.0}};
    for (const bool draw : {false, true}) {
      const std::vector<Rating> rated =
          RateFreeForAll(ratings, std::vector{0, draw ? 0 : 1}, params);
      const auto [a, b] = Rate1v1(ratings[0], ratings[1], draw, params);
      EXPECT_NEAR(rated[0].mu, a.mu, 1e-9);
      EXPECT_NEAR(rated[0].sigma, a.sigma, 1e-9);
      EXPECT_NEAR(rated[1].mu, b.mu, 1e-9);
      EXPECT_NEAR(rated[1].sigma, b.sigma, 1e-9);
    }
  }
}

// Reference values: trueskill.TrueSkill().rate([(r,) for r in ...], ranks)
// of the Python package, draw_probability as noted.
TEST(RateFreeForAllTest, MatchesReference) {
  const Params params;
  const Rating fresh = params.Initial();
  const std::vector<Rating> three = {fresh, fresh, fresh};
  const std::vector<Rating> mixed = {{30.0, 4.0}, {20.0, 5.0}, {25.0, 3.0}};
  struct Case {
    std::vector<Rating> ratings;
    std::vector<int> places;
    std::vector<Rating> expected;
  };
  const Case cases[] = {
      {three,
       {0, 1, 2},
       {{31.675352, 6.655985}, {25.0, 6.207897}, {18.324648, 6.655985}}},
      {three,
       {0, 0, 1},
       {{27.55196, 5.97413}, {27.557338, 5.972007}, {19.890701, 6.735245}}},
      {mixed,
       {2, 0, 1},
       {{25.931499, 3.435828}, {25.829133, 4.015526}, {25.18978, 2.71974}}},
  };
  for (const Case &c : cases) {
    const std::vector<Rating> rated =
        RateFreeForAll(c.ratings, c.places, params);
    for (std::size_t i = 0; i < rated.size(); ++i) {
      EXPECT_NEAR(rated[i].mu, c.expected[i].mu, 1e-4) << i;
      EXPECT_NEAR(rated[i].sigma, c.expected[i].sigma, 1e-4) << i;
    }
  }
  Params no_draws;
  no_draws.draw_probability = 0.0;
  const std::vector<Rating> rated =
      RateFreeForAll(three, std::vector{0, 1, 2}, no_draws);
  EXPECT_NEAR(rated[0].mu, 31.311737, 1e-4);
  EXPECT_NEAR(rated[1].sigma, 6.238733, 1e-4);
  EXPECT_NEAR(rated[2].mu, 18.688263, 1e-4);
}

TEST(RateFreeForAllTest, TieWithoutDrawMarginStaysFinite) {
  Params params;
  params.draw_probability = 0.0;
  const std::vector<Rating> ratings = {{30.0, 4.0}, {20.0, 5.0}, {25.0, 3.0}};
  const std::vector<Rating> rated =
      RateFreeForAll(ratings, std::vector{0, 1, 1}, params);
  for (const Rating &r : rated) {
    EXPECT_TRUE(std::isfinite(r.mu));
    EXPECT_GT(r.sigma, 0.0);
  }
  EXPECT_GT(rated[0].mu, rated[2].mu);
  EXPECT_LT(rated[1].mu - rated[2].mu, 30.0 - 20.0);
}

TEST(RateFreeForAllTest, ExtremeMismatchStaysFinite) {
  const Params params;
  const std::vector<Rating> ratings = {{200.0, 1.0}, {0.0, 1.0}, {100.0, 1.0}};
  for (const std::vector<int> &places :
       {std::vector{0, 2, 1}, std::vector{2, 0, 1}, std::vector{1, 1, 0}}) {
    for (const Rating &r : RateFreeForAll(ratings, places, params)) {
      EXPECT_TRUE(std::isfinite(r.mu));
      EXPECT_TRUE(std::isfinite(r.sigma));
      EXPECT_GT(r.sigma, 0.0);
    }
  }
}

// Reference values: trueskill.TrueSkill().quality(...) of the Python package.
TEST(QualityTest, MatchesReference) {
  const double beta = Params{}.beta;
  EXPECT_NEAR(Quality(std::vector<Rating>{{30.0, 4.0}, {20.0, 5.0}}, beta),
              0.349883851, 1e-9);
  EXPECT_NEAR(
      Quality(std::vector<Rating>{{30.0, 4.0}, {20.0, 5.0}, {25.0, 3.0}}, beta),
      0.265052436, 1e-9);
  // Order-free: the same players, listed differently.
  EXPECT_NEAR(
      Quality(std::vector<Rating>{{25.0, 3.0}, {30.0, 4.0}, {20.0, 5.0}}, beta),
      0.265052436, 1e-9);
  EXPECT_NEAR(
      Quality(std::vector<Rating>{{25.0, 0.0}, {25.0, 0.0}, {25.0, 0.0}}, beta),
      1.0, 1e-12);
}

TEST(WinProbabilityTest, EqualPlayers) {
  Params params;
  params.draw_probability = 0.0;
  EXPECT_NEAR(WinProbability(params.Initial(), params.Initial(), params), 0.5,
              1e-12);
  // draw_probability is defined between players of exactly known, equal
  // skill: each side wins 45%, the rest is the draw.
  params.draw_probability = 0.1;
  const Rating known{25.0, 0.0};
  EXPECT_NEAR(WinProbability(known, known, params), 0.45, 1e-9);
}

TEST(RankerTest, CountsWhoWonFromEitherSeat) {
  Ranker ranker{Params{}};
  const std::vector<std::string> strong_first = {"strong", "weak"};
  const std::vector<std::string> weak_first = {"weak", "strong"};
  for (int i = 0; i < 10; ++i) {
    ranker.AddGame(strong_first, std::vector{0, 1});
    ranker.AddGame(weak_first, std::vector{1, 0});
  }
  ranker.AddGame(weak_first, std::vector{0, 0});
  const PlayerRecord strong = ranker.Get("strong");
  const PlayerRecord weak = ranker.Get("weak");
  EXPECT_EQ(strong.wins, 20);
  EXPECT_EQ(strong.losses, 0);
  EXPECT_EQ(strong.draws, 1);
  EXPECT_EQ(weak.losses, 20);
  EXPECT_GT(strong.rating.Conservative(), weak.rating.Conservative());
}

TEST(RankerTest, CountsFinishesOfAFreeForAll) {
  Ranker ranker{Params{}};
  const std::vector<std::string> players = {"a", "b", "c"};
  ranker.AddGame(players, std::vector{2, 0, 1});
  ranker.AddGame(players, std::vector{0, 0, 2});
  const PlayerRecord a = ranker.Get("a");
  const PlayerRecord c = ranker.Get("c");
  EXPECT_EQ(a.finishes, (std::vector{1, 0, 1}));
  EXPECT_EQ(a.wins + a.draws + a.losses, 2);
  EXPECT_EQ(a.draws, 1);
  EXPECT_EQ(c.finishes, (std::vector{0, 1, 1}));
  EXPECT_EQ(c.losses, 2);
  EXPECT_EQ(a.met, (std::map<std::string, int>{{"b", 2}, {"c", 2}}));
  EXPECT_GT(ranker.Get("b").rating.mu, c.rating.mu);
}

TEST(RankerTest, UnseenPlayerIsInitial) {
  const Ranker ranker{Params{}};
  const PlayerRecord nobody = ranker.Get("nobody");
  EXPECT_DOUBLE_EQ(nobody.rating.mu, 25.0);
  EXPECT_DOUBLE_EQ(nobody.rating.sigma, 25.0 / 3.0);
  EXPECT_EQ(nobody.wins + nobody.draws + nobody.losses, 0);
}

}  // namespace
}  // namespace tournament_broker::trueskill
