#include "game_mcts/tournament_server/server/elo_standings.h"

#include <algorithm>
#include <string>
#include <utility>

#include "absl/log/log.h"

namespace tournament_arena {

namespace {

constexpr std::string_view kPlayerPrefix = "player:";

// The name the opponent is rated under. A builtin is a player like any other --
// that is what makes "beat builtin:mcts" a meaningful thing to be rated for --
// and a candidate opponent is rated under its own id.
auto OpponentName(const std::string &opponent) -> std::string {
  if (opponent.rfind(kPlayerPrefix, 0) == 0) {
    return opponent.substr(kPlayerPrefix.size());
  }
  return opponent;
}

}  // namespace

EloStandings::EloStandings(tournament_broker::EloStore *elo_store,
                           const CandidateStore *candidates,
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

  // One call per game. The whole tally applied at once would give a different
  // rating: ELO's update depends on the rating at the time of each game, so
  // six wins then four losses is not the same as ten games averaged.
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

auto EloStandings::Get(const std::string &candidate_id) const -> Standing {
  const auto rating = elo_store_->Get(problem_id_, candidate_id);
  Standing standing;
  standing.candidate_id = candidate_id;
  standing.score = rating.elo();
  standing.wins = rating.wins();
  standing.draws = rating.draws();
  standing.losses = rating.losses();
  return standing;
}

auto EloStandings::has(const std::string &candidate_id) const -> bool {
  const auto rating = elo_store_->Get(problem_id_, candidate_id);
  return rating.wins() + rating.draws() + rating.losses() > 0;
}

auto EloStandings::Rank(int limit) const -> std::vector<Standing> {
  std::vector<Standing> rows;
  if (candidates_ != nullptr) {
    for (const proto::Candidate &candidate : candidates_->List()) {
      if (candidate.status() != proto::Candidate::READY) {
        continue;
      }
      rows.push_back(Get(candidate.candidate_id()));
    }
  } else {
    // No submission registry: rank whoever has played. The rating store keys
    // are "<problem>\t<player>".
    const tournament_broker::proto::RatingStore snapshot =
        elo_store_->Snapshot();
    for (const auto &[key, rating] : snapshot.ratings()) {
      const auto tab = key.find('\t');
      if (tab == std::string::npos || key.substr(0, tab) != problem_id_) {
        continue;
      }
      rows.push_back(Get(key.substr(tab + 1)));
    }
  }
  std::sort(rows.begin(), rows.end(), [](const Standing &a, const Standing &b) {
    if (a.score != b.score) {
      return a.score > b.score;
    }
    // A stable tiebreak, so two equally rated rows do not swap
    // places between requests.
    return a.candidate_id < b.candidate_id;
  });
  if (limit > 0 && rows.size() > static_cast<std::size_t>(limit)) {
    rows.resize(limit);
  }
  return rows;
}

}  // namespace tournament_arena
