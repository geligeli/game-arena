#ifndef GAME_ARENA_GAME_ARENA_STANDINGS_CANDIDATE_VIEW_H
#define GAME_ARENA_GAME_ARENA_STANDINGS_CANDIDATE_VIEW_H

// Reading candidates, for the things that only read them.
//
// The standings and the leaderboard show who submitted what. They do not
// store submissions, validate patches or generate BUILD files, and depending
// on the thing that does drags all of that behind them -- which is how a
// participant's local broker came to link the coordinator's submission
// machinery. This is the half they actually use.
//
// CandidateStore implements it. A local broker passes nullptr: it rates
// whoever plays, and nobody there has submitted anything.

#include <optional>
#include <string>
#include <vector>

#include "game_arena/proto/arena.pb.h"

namespace tournament_arena {

class CandidateView {
 public:
  virtual ~CandidateView() = default;

  // Newest first.
  virtual std::vector<proto::Candidate> List() const = 0;

  virtual std::optional<proto::Candidate> Get(
      const std::string &candidate_id) const = 0;
};

}  // namespace tournament_arena

#endif  // GAME_ARENA_GAME_ARENA_STANDINGS_CANDIDATE_VIEW_H
