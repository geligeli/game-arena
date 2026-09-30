#ifndef GAME_ARENA_GAME_ARENA_STANDINGS_TRUESKILL_H
#define GAME_ARENA_GAME_ARENA_STANDINGS_TRUESKILL_H

// TrueSkill (Herbrich, Minka, Graepel 2006) for one-player sides: a Gaussian
// belief (mu, sigma) per player, updated after every game, for two players or
// a free-for-all among more.

#include <map>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace tournament_broker::trueskill {

struct Rating {
  double mu = 0.0;
  double sigma = 0.0;

  // The usual leaderboard key: a skill the player very likely exceeds.
  double Conservative(double k = 3.0) const { return mu - k * sigma; }
};

// The defaults of the paper and of the reference Python package.
struct Params {
  double mu0 = 25.0;
  double sigma0 = 25.0 / 3.0;
  // Performance noise: the skill gap that gives ~76% to the stronger side.
  double beta = 25.0 / 6.0;
  // Added to sigma before every game so a player whose skill changes (a bot
  // resubmitted) is not frozen by an ever smaller sigma.
  double tau = 25.0 / 300.0;
  // How often two equal players draw; 0 for a game that cannot draw.
  double draw_probability = 0.1;

  Rating Initial() const { return {mu0, sigma0}; }
};

// The performance gap below which a game is a draw.
double DrawMargin(const Params &params);

// Ratings after a game, in argument order. With |draw| the order is
// irrelevant; a draw needs no positive draw_probability to be rated.
std::pair<Rating, Rating> Rate1v1(const Rating &winner, const Rating &loser,
                                  bool draw, const Params &params);

// Ratings after a free-for-all, in argument order. |places| per player: 0 is
// first, equal places tie. Message passing is scheduled as the reference
// Python package does it, so the two agree.
std::vector<Rating> RateFreeForAll(std::span<const Rating> ratings,
                                   std::span<const int> places,
                                   const Params &params);

// P(|a| beats |b| outright); draws take what is left of 1 - P(b beats a).
double WinProbability(const Rating &a, const Rating &b, const Params &params);

// The chance of a draw among |ratings|, relative to the most even game
// possible: 1 for equal, exactly known skills.
double Quality(std::span<const Rating> ratings, double beta);

struct PlayerRecord {
  Rating rating;
  // A sole first is a win, a shared first a draw, anything else a loss.
  int wins = 0;
  int draws = 0;
  int losses = 0;
  std::vector<int> finishes = {};  // games per place, first first
};

// Ratings by player name, fed one game at a time in the order played.
// Not thread-safe.
class Ranker {
 public:
  explicit Ranker(Params params) : params_(params) {}

  // |places| by seat, as RateFreeForAll takes them.
  void AddGame(std::span<const std::string> players,
               std::span<const int> places);
  // An unseen player is at the initial rating with no games.
  PlayerRecord Get(const std::string &player) const;

 private:
  Params params_;
  std::map<std::string, PlayerRecord> players_;
};

}  // namespace tournament_broker::trueskill

#endif  // GAME_ARENA_GAME_ARENA_STANDINGS_TRUESKILL_H
