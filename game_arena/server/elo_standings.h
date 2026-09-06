#ifndef GAME_ARENA_GAME_ARENA_SERVER_ELO_STANDINGS_H
#define GAME_ARENA_GAME_ARENA_SERVER_ELO_STANDINGS_H

// Standings for a match problem: ELO over the existing per-(problem, player)
// store.
//
// Record() turns a referee's tally back into per-game ELO updates. Doing it
// game by game rather than once for the whole tally is what makes the result
// identical to the in-process broker's, which recorded each game as it
// finished -- ELO is path dependent, so a 6-4 result reached in a different
// order is a different rating.

#include <string>
#include <vector>

#include "game_arena/server/candidate_store.h"
#include "game_arena/server/elo_store.h"
#include "game_arena/server/standings.h"

namespace tournament_arena {

class EloStandings final : public Standings {
 public:
  // |problem_id| keys the rating store, the way the game name used to.
  //
  // |candidates| may be null. With one, Rank() lists the arena's READY
  // submissions -- the arena's leaderboard is about submissions, and a
  // half-built one has no business on it. Without one, it lists every player
  // the rating store has seen, which is what the standalone broker's dev loop
  // wants: ad-hoc clients and builtins included.
  EloStandings(tournament_broker::EloStore *elo_store,
               const CandidateStore *candidates, std::string problem_id);

  void Record(const std::string &candidate_id, const std::string &opponent,
              const proto::OrderResult &result) override;
  auto Get(const std::string &candidate_id) const -> Standing override;
  auto Rank(int limit) const -> std::vector<Standing> override;
  auto score_label() const -> std::string override { return "elo"; }
  auto has(const std::string &candidate_id) const -> bool override;

 private:
  // Get() for a rating filed under a problem other than problem_id_. Only
  // Rank() with an empty problem_id_ needs it: there the problem comes from
  // each rating store key rather than from this object.
  auto GetIn(const std::string &problem_id,
             const std::string &candidate_id) const -> Standing;

  tournament_broker::EloStore *elo_store_;  // not owned
  const CandidateStore *candidates_;        // not owned
  const std::string problem_id_;
};

}  // namespace tournament_arena

#endif  // GAME_ARENA_GAME_ARENA_SERVER_ELO_STANDINGS_H
