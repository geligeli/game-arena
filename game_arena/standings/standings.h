#ifndef GAME_ARENA_GAME_ARENA_STANDINGS_STANDINGS_H
#define GAME_ARENA_GAME_ARENA_STANDINGS_STANDINGS_H

// How a problem's submissions are scored and ordered: ELO, TrueSkill or a
// measured metric. Only the coordinator writes standings, from the tallies
// orders report and, for TrueSkill, from each game as it is stored.

#include <algorithm>
#include <cstddef>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "game_arena/proto/arena.pb.h"
#include "game_arena/proto/tournament_broker.pb.h"

namespace tournament_arena {

struct Standing {
  std::string candidate_id;
  double score = 0.0;  // what Rank() orders by, in the metric's direction

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

  // |opponent| is the tally's other side ("builtin:random" or a candidate id);
  // a graded problem ignores it.
  virtual void Record(const std::string &candidate_id,
                      const std::string &opponent,
                      const proto::OrderResult &result) = 0;

  // Every stored game of a match, in the order stored. For ratings that need
  // the real sequence of games rather than an order's tally.
  virtual void RecordGame(
      const tournament_broker::proto::GameRecord & /*record*/) {}

  virtual Standing Get(const std::string &candidate_id) const = 0;

  // Best first. |limit| <= 0 returns everyone.
  virtual std::vector<Standing> Rank(int limit) const = 0;

  // The score column's heading.
  virtual std::string score_label() const = 0;

  // Measured at all: a board must not show a default rating as a result.
  virtual bool has(const std::string &candidate_id) const = 0;

  // A measured metric (runs, machine) rather than a match record (W/D/L).
  virtual bool graded() const { return false; }
};

// The READY submissions among |candidates|, best first by |standings|; ties go
// by id so equal rows do not swap places between requests. |limit| <= 0
// returns everyone.
inline std::vector<Standing> RankReady(
    const std::vector<proto::Candidate> &candidates,
    const Standings &standings, int limit) {
  std::vector<Standing> rows;
  for (const proto::Candidate &candidate : candidates) {
    if (candidate.status() == proto::Candidate::READY) {
      rows.push_back(standings.Get(candidate.candidate_id()));
    }
  }
  std::ranges::sort(rows, {}, [](const Standing &row) {
    return std::pair<double, const std::string &>(-row.score, row.candidate_id);
  });
  if (limit > 0 && rows.size() > static_cast<std::size_t>(limit)) {
    rows.resize(limit);
  }
  return rows;
}

}  // namespace tournament_arena

#endif  // GAME_ARENA_GAME_ARENA_STANDINGS_STANDINGS_H
