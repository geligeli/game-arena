#include "game_arena/standings/trueskill.h"

#include <cmath>
#include <utility>

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
  for (int i = 0; i < 10; ++i) {
    ranker.AddGame("strong", "weak", /*winner=*/0);
    ranker.AddGame("weak", "strong", /*winner=*/1);
  }
  ranker.AddGame("weak", "strong", /*winner=*/-1);
  const PlayerRecord strong = ranker.Get("strong");
  const PlayerRecord weak = ranker.Get("weak");
  EXPECT_EQ(strong.wins, 20);
  EXPECT_EQ(strong.losses, 0);
  EXPECT_EQ(strong.draws, 1);
  EXPECT_EQ(weak.losses, 20);
  EXPECT_GT(strong.rating.Conservative(), weak.rating.Conservative());
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
