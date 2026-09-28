#include "game_arena/standings/trueskill.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <tuple>

namespace tournament_broker::trueskill {

namespace {

double NormalPdf(double x) {
  return std::exp(-0.5 * x * x) / std::sqrt(2.0 * std::numbers::pi);
}

double NormalCdf(double x) { return 0.5 * std::erfc(-x / std::numbers::sqrt2); }

// v and w of the paper: the mean shift and variance shrink factors of a
// truncated Gaussian, for a win by at least |margin| and for a draw.
struct Truncation {
  double v;
  double w;
};

Truncation WinTruncation(double t, double margin) {
  const double x = t - margin;
  double v;
  if (x > -30.0) {
    v = NormalPdf(x) / NormalCdf(x);
  } else {
    // Mills ratio: the cdf underflows long before the ratio misbehaves.
    const double x2 = x * x;
    v = -x / (1.0 - 1.0 / x2 + 3.0 / (x2 * x2));
  }
  return {v, std::clamp(v * (v + x), 0.0, 1.0)};
}

Truncation DrawTruncation(double t, double margin) {
  const double abs_t = std::abs(t);
  const double a = margin - abs_t;
  const double b = -margin - abs_t;
  const double mass = NormalCdf(a) - NormalCdf(b);
  // No margin (or a hopeless mismatch): the limit of both as mass -> 0.
  if (mass < 1e-300) return {-t, 1.0};
  const double v = (NormalPdf(b) - NormalPdf(a)) / mass * (t < 0 ? -1.0 : 1.0);
  const double w = v * v + (a * NormalPdf(a) - b * NormalPdf(b)) / mass;
  return {v, std::clamp(w, 0.0, 1.0)};
}

}  // namespace

double DrawMargin(const Params &params) {
  // Inverse cdf by bisection: exact to double precision, no approximation's
  // coefficient tables.
  const double target = (params.draw_probability + 1.0) / 2.0;
  double lo = 0.0;
  double hi = 40.0;
  for (int i = 0; i < 200; ++i) {
    const double mid = 0.5 * (lo + hi);
    (NormalCdf(mid) < target ? lo : hi) = mid;
  }
  return 0.5 * (lo + hi) * std::numbers::sqrt2 * params.beta;
}

std::pair<Rating, Rating> Rate1v1(const Rating &winner, const Rating &loser,
                                  bool draw, const Params &params) {
  const double tau2 = params.tau * params.tau;
  const double w_var = winner.sigma * winner.sigma + tau2;
  const double l_var = loser.sigma * loser.sigma + tau2;
  const double c2 = 2.0 * params.beta * params.beta + w_var + l_var;
  const double c = std::sqrt(c2);
  const double t = (winner.mu - loser.mu) / c;
  const double margin = DrawMargin(params) / c;
  const auto [v, w] =
      draw ? DrawTruncation(t, margin) : WinTruncation(t, margin);
  const auto update = [&](double mu, double var, double sign) {
    return Rating{mu + sign * var / c * v,
                  std::sqrt(var * (1.0 - var / c2 * w))};
  };
  return {update(winner.mu, w_var, 1.0), update(loser.mu, l_var, -1.0)};
}

double WinProbability(const Rating &a, const Rating &b, const Params &params) {
  const double c = std::sqrt(2.0 * params.beta * params.beta +
                             a.sigma * a.sigma + b.sigma * b.sigma);
  return NormalCdf((a.mu - b.mu - DrawMargin(params)) / c);
}

void Ranker::AddGame(const std::string &player0, const std::string &player1,
                     int winner) {
  const PlayerRecord initial{params_.Initial()};
  PlayerRecord &p0 = players_.try_emplace(player0, initial).first->second;
  PlayerRecord &p1 = players_.try_emplace(player1, initial).first->second;
  const bool draw = winner < 0;
  PlayerRecord &won = winner == 1 ? p1 : p0;
  PlayerRecord &lost = winner == 1 ? p0 : p1;
  std::tie(won.rating, lost.rating) =
      Rate1v1(won.rating, lost.rating, draw, params_);
  if (draw) {
    ++p0.draws;
    ++p1.draws;
  } else {
    ++won.wins;
    ++lost.losses;
  }
}

PlayerRecord Ranker::Get(const std::string &player) const {
  const auto it = players_.find(player);
  return it != players_.end() ? it->second : PlayerRecord{params_.Initial()};
}

}  // namespace tournament_broker::trueskill
