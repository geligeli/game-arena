#ifndef GAME_ARENA_GAME_ARENA_STANDINGS_CANDIDATE_VIEW_H
#define GAME_ARENA_GAME_ARENA_STANDINGS_CANDIDATE_VIEW_H

// The read half of CandidateStore, so readers need not link the store.

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
