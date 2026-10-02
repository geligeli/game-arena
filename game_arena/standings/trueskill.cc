#include "game_arena/standings/trueskill.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <numbers>
#include <numeric>

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

// A message of the factor graph, in the natural parameters it is multiplied
// and divided in.
struct Gaussian {
  double pi = 0.0;  // precision; 0 is uniform
  double tau = 0.0;

  static Gaussian Of(double mu, double var) { return {1.0 / var, mu / var}; }
  double mu() const { return pi == 0.0 ? 0.0 : tau / pi; }
  Gaussian operator*(const Gaussian &o) const {
    return {pi + o.pi, tau + o.tau};
  }
  Gaussian operator/(const Gaussian &o) const {
    return {pi - o.pi, tau - o.tau};
  }
};

// x + sign * y, uniform when either is.
Gaussian Sum(const Gaussian &x, const Gaussian &y, double sign) {
  if (x.pi == 0.0 || y.pi == 0.0) {
    return {};
  }
  return Gaussian::Of(x.mu() + sign * y.mu(), 1.0 / x.pi + 1.0 / y.pi);
}

double Delta(const Gaussian &a, const Gaussian &b) {
  const double pi_delta = std::abs(a.pi - b.pi);
  return std::isinf(pi_delta)
             ? 0.0
             : std::max(std::abs(a.tau - b.tau), std::sqrt(pi_delta));
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

std::vector<Rating> RateFreeForAll(std::span<const Rating> ratings,
                                   std::span<const int> places,
                                   const Params &params) {
  const std::size_t n = ratings.size();
  std::vector<std::size_t> order(n);
  std::iota(order.begin(), order.end(), std::size_t{0});
  std::ranges::stable_sort(order, {}, [&](std::size_t i) { return places[i]; });
  const double tau2 = params.tau * params.tau;
  const double beta2 = params.beta * params.beta;
  const double margin = DrawMargin(params);

  // Performances best first; the differences of neighbours, perf[k] -
  // perf[k + 1], with the messages their factors sent.
  std::vector<Gaussian> skill(n), prior(n), perf(n);
  for (std::size_t k = 0; k < n; ++k) {
    const Rating &r = ratings[order[k]];
    skill[k] = Gaussian::Of(r.mu, r.sigma * r.sigma + tau2);
    prior[k] = perf[k] = Gaussian::Of(r.mu, r.sigma * r.sigma + tau2 + beta2);
  }
  const std::size_t m = n - 1;
  std::vector<Gaussian> diff(m), to_diff(m), to_left(m), to_right(m), trunc(m);
  const auto down = [&](std::size_t k) {
    const Gaussian msg =
        Sum(perf[k] / to_left[k], perf[k + 1] / to_right[k], -1.0);
    diff[k] = diff[k] / to_diff[k] * msg;
    to_diff[k] = msg;
  };
  const auto truncate = [&](std::size_t k) {
    const Gaussian div = diff[k] / trunc[k];
    const double sqrt_pi = std::sqrt(div.pi);
    const double t = div.tau / sqrt_pi;
    const auto [v, w] = places[order[k]] == places[order[k + 1]]
                            ? DrawTruncation(t, margin * sqrt_pi)
                            : WinTruncation(t, margin * sqrt_pi);
    // A tie without a draw margin pins the difference at 0: w is 1.
    const double denom = std::max(1.0 - w, 1e-12);
    const Gaussian updated{div.pi / denom, (div.tau + sqrt_pi * v) / denom};
    const double delta = Delta(diff[k], updated);
    trunc[k] = updated / div;
    diff[k] = updated;
    return delta;
  };
  const auto up_right = [&](std::size_t k) {
    const Gaussian msg = Sum(perf[k] / to_left[k], diff[k] / to_diff[k], -1.0);
    perf[k + 1] = perf[k + 1] / to_right[k] * msg;
    to_right[k] = msg;
  };
  const auto up_left = [&](std::size_t k) {
    const Gaussian msg =
        Sum(diff[k] / to_diff[k], perf[k + 1] / to_right[k], 1.0);
    perf[k] = perf[k] / to_left[k] * msg;
    to_left[k] = msg;
  };
  for (int iteration = 0; iteration < 10; ++iteration) {
    double delta = 0.0;
    if (m == 1) {
      down(0);
      delta = truncate(0);
    }
    for (std::size_t k = 0; m > 1 && k + 1 < m; ++k) {
      down(k);
      delta = std::max(delta, truncate(k));
      up_right(k);
    }
    for (std::size_t k = m - 1; m > 1 && k > 0; --k) {
      down(k);
      delta = std::max(delta, truncate(k));
      up_left(k);
    }
    if (delta <= 1e-4) {
      break;
    }
  }
  up_left(0);
  up_right(m - 1);

  std::vector<Rating> rated(n);
  for (std::size_t k = 0; k < n; ++k) {
    // What the game says of the performance, blurred by beta into the skill.
    const Gaussian evidence = perf[k] / prior[k];
    const double a = 1.0 / (1.0 + beta2 * evidence.pi);
    const Gaussian posterior =
        skill[k] * Gaussian{a * evidence.pi, a * evidence.tau};
    rated[order[k]] = {posterior.mu(), std::sqrt(1.0 / posterior.pi)};
  }
  return rated;
}

double WinProbability(const Rating &a, const Rating &b, const Params &params) {
  const double c = std::sqrt(2.0 * params.beta * params.beta +
                             a.sigma * a.sigma + b.sigma * b.sigma);
  return NormalCdf((a.mu - b.mu - DrawMargin(params)) / c);
}

double Quality(std::span<const Rating> ratings, double beta) {
  // Over the differences of neighbours both matrices of the paper's formula
  // are tridiagonal: an LDL^T gives the determinant and the quadratic form.
  const double beta2 = beta * beta;
  double det = 1.0, quad = 0.0, d = 0.0, y = 0.0;
  for (std::size_t k = 0; k + 1 < ratings.size(); ++k) {
    const double s0 = ratings[k].sigma * ratings[k].sigma;
    const double s1 = ratings[k + 1].sigma * ratings[k + 1].sigma;
    double a = 2 * beta2 + s0 + s1;
    double r = ratings[k].mu - ratings[k + 1].mu;
    if (k > 0) {
      const double b = -(beta2 + s0);
      a -= b * b / d;
      r -= b / d * y;
    }
    d = a;
    y = r;
    det *= d;
    quad += y * y / d;
  }
  const auto n = static_cast<double>(ratings.size());
  return std::sqrt(std::pow(beta2, n - 1) * n / det) * std::exp(-quad / 2);
}

void Ranker::AddGame(std::span<const std::string> players,
                     std::span<const int> places) {
  std::vector<PlayerRecord *> records;
  std::vector<Rating> ratings;
  for (const std::string &player : players) {
    records.push_back(
        &players_.try_emplace(player, PlayerRecord{params_.Initial()})
             .first->second);
    ratings.push_back(records.back()->rating);
  }
  std::vector<Rating> rated;
  if (players.size() == 2) {
    // The closed form, so ratings from before free-for-alls replay unchanged.
    const bool second = places[1] < places[0];
    const auto [won, lost] = Rate1v1(ratings[second], ratings[!second],
                                     places[0] == places[1], params_);
    rated = second ? std::vector{lost, won} : std::vector{won, lost};
  } else {
    rated = RateFreeForAll(ratings, places, params_);
  }
  const int first = std::ranges::min(places);
  const bool shared = std::ranges::count(places, first) > 1;
  for (std::size_t i = 0; i < records.size(); ++i) {
    PlayerRecord &record = *records[i];
    record.rating = rated[i];
    const auto place = static_cast<std::size_t>(places[i]);
    if (record.finishes.size() <= place) {
      record.finishes.resize(place + 1);
    }
    ++record.finishes[place];
    for (const std::string &opponent : players) {
      if (opponent != players[i]) {
        ++record.met[opponent];
      }
    }
    ++(places[i] != first ? record.losses
       : shared           ? record.draws
                          : record.wins);
  }
}

PlayerRecord Ranker::Get(const std::string &player) const {
  const auto it = players_.find(player);
  return it != players_.end() ? it->second : PlayerRecord{params_.Initial()};
}

}  // namespace tournament_broker::trueskill
