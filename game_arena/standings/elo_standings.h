#ifndef GAME_ARENA_GAME_ARENA_STANDINGS_ELO_STANDINGS_H
#define GAME_ARENA_GAME_ARENA_STANDINGS_ELO_STANDINGS_H

// Standings for a match problem: ELO per (problem, player).

#include <string>
#include <vector>

#include "game_arena/standings/candidate_view.h"
#include "game_arena/standings/elo_store.h"
#include "game_arena/standings/standings.h"

namespace tournament_arena {

class EloStandings final : public Standings {
 public:
  // Rank() lists only |candidates|' READY submissions.
  EloStandings(tournament_broker::EloStore *elo_store,
               const CandidateView *candidates, std::string problem_id);

  void Record(const std::string &candidate_id, const std::string &opponent,
              const proto::OrderResult &result) override;
  Standing Get(const std::string &candidate_id) const override;
  std::vector<Standing> Rank(int limit) const override;
  std::string score_label() const override { return "elo"; }
  bool has(const std::string &candidate_id) const override;

 private:
  tournament_broker::EloStore *elo_store_;  // not owned
  const CandidateView *candidates_;         // not owned
  const std::string problem_id_;
};

}  // namespace tournament_arena

#endif  // GAME_ARENA_GAME_ARENA_STANDINGS_ELO_STANDINGS_H
