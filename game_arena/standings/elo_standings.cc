#include "game_arena/standings/elo_standings.h"

#include <string>
#include <utility>

#include "absl/log/log.h"

namespace tournament_arena {

namespace {

constexpr std::string_view kPlayerPrefix = "player:";

// A builtin is rated like any player; a candidate under its own id.
std::string OpponentName(const std::string &opponent) {
  if (opponent.rfind(kPlayerPrefix, 0) == 0) {
    return opponent.substr(kPlayerPrefix.size());
  }
  return opponent;
}

}  // namespace

EloStandings::EloStandings(tournament_broker::EloStore *elo_store,
                           const CandidateView *candidates,
                           std::string problem_id)
    : elo_store_(elo_store),
      candidates_(candidates),
      problem_id_(std::move(problem_id)) {}

void EloStandings::Record(const std::string &candidate_id,
                          const std::string &opponent,
                          const proto::OrderResult &result) {
  if (opponent.empty()) {
    LOG(WARNING) << "Ignoring a tally for " << candidate_id
                 << " with no opponent to rate it against";
    return;
  }
  const std::string rival = OpponentName(opponent);

  // One update per game: ELO is path dependent, so the tally at once differs.
  for (int i = 0; i < result.wins(); ++i) {
    elo_store_->RecordResult(problem_id_, candidate_id, rival, 1.0);
  }
  for (int i = 0; i < result.draws(); ++i) {
    elo_store_->RecordResult(problem_id_, candidate_id, rival, 0.5);
  }
  for (int i = 0; i < result.losses(); ++i) {
    elo_store_->RecordResult(problem_id_, candidate_id, rival, 0.0);
  }
}

Standing EloStandings::Get(const std::string &candidate_id) const {
  const auto rating = elo_store_->Get(problem_id_, candidate_id);
  Standing standing;
  standing.candidate_id = candidate_id;
  standing.score = rating.elo();
  standing.wins = rating.wins();
  standing.draws = rating.draws();
  standing.losses = rating.losses();
  return standing;
}

bool EloStandings::has(const std::string &candidate_id) const {
  const auto rating = elo_store_->Get(problem_id_, candidate_id);
  return rating.wins() + rating.draws() + rating.losses() > 0;
}

std::vector<Standing> EloStandings::Rank(int limit) const {
  return RankReady(candidates_->List(), *this, limit);
}

}  // namespace tournament_arena
