#ifndef GAME_ARENA_GAME_ARENA_SERVER_STANDINGS_H
#define GAME_ARENA_GAME_ARENA_SERVER_STANDINGS_H

// How a problem's submissions are scored and ordered.
//
// Two problems ask genuinely different questions -- "who beats whom" and "how
// fast is it" -- and only one of them has an opponent. Rather than teach the
// scheduler, the Arena service and the leaderboard about both, they all go
// through this: record an order's result, ask for the standings, render what
// comes back.
//
// The coordinator is the only thing that writes standings. A match's referee
// keeps its own ratings while it plays, but they die with the container: they
// exist so a game has somewhere to record itself, not to be authoritative. What
// crosses the wire is a tally, and this turns the tally into a rating here,
// once, on the machine that owns the store.

#include <map>
#include <string>
#include <vector>

#include "game_arena/proto/arena.pb.h"

namespace tournament_arena {

// One row of a leaderboard.
struct Standing {
  std::string candidate_id;
  // What the board is ordered by. Higher is better after Rank() has applied the
  // metric's direction, so callers never re-derive which way round it goes.
  double score = 0.0;

  // Match problems.
  int wins = 0;
  int draws = 0;
  int losses = 0;

  // Graded problems.
  std::map<std::string, double> metrics;
  std::string worker_id;
  std::string machine_class;
  int runs = 0;
};

class Standings {
 public:
  virtual ~Standings() = default;

  // Folds one finished order into the standings. |opponent| is the tally's
  // other side ("builtin:random", or a candidate id); it is meaningless for a
  // graded problem and ignored there.
  virtual void Record(const std::string &candidate_id,
                      const std::string &opponent,
                      const proto::OrderResult &result) = 0;

  virtual auto Get(const std::string &candidate_id) const -> Standing = 0;

  // Best first. |limit| <= 0 returns everyone.
  virtual auto Rank(int limit) const -> std::vector<Standing> = 0;

  // What the score column is called, for the HTTP table and the JSON key.
  virtual auto score_label() const -> std::string = 0;

  // True when a submission has been measured at all. A board should not show a
  // default rating as if it were a result.
  virtual auto has(const std::string &candidate_id) const -> bool = 0;
};

}  // namespace tournament_arena

#endif  // GAME_ARENA_GAME_ARENA_SERVER_STANDINGS_H
