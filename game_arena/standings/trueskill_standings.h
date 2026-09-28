#ifndef GAME_ARENA_GAME_ARENA_STANDINGS_TRUESKILL_STANDINGS_H
#define GAME_ARENA_GAME_ARENA_STANDINGS_TRUESKILL_STANDINGS_H

// Standings for a match problem ranked by TrueSkill (score: mu - 3 sigma).
// Rated game by game in the order the coordinator stores games, and rebuilt
// at start by replaying the game index, so there is no ratings file: the
// ratings are always a replay of <data_dir>/games/index.jsonl. That includes
// the games of an order that ended in an error (a short match), which ELO,
// rating only an order's final tally, leaves out.

#include <mutex>
#include <string>
#include <vector>

#include "game_arena/standings/candidate_view.h"
#include "game_arena/standings/game_history.h"
#include "game_arena/standings/standings.h"
#include "game_arena/standings/trueskill.h"

namespace tournament_arena {

class TrueSkillStandings final : public Standings {
 public:
  // Replays |history|. Rank() lists only |candidates|' READY submissions:
  // builtins are rated but never ranked, since the ladder of rivals is built
  // from Rank().
  TrueSkillStandings(const tournament_broker::GameHistory &history,
                     const CandidateView *candidates,
                     tournament_broker::trueskill::Params params);

  // Rated per game by RecordGame; the tally adds nothing.
  void Record(const std::string & /*candidate_id*/,
              const std::string & /*opponent*/,
              const proto::OrderResult & /*result*/) override {}
  void RecordGame(const tournament_broker::proto::GameRecord &record) override;
  Standing Get(const std::string &candidate_id) const override;
  // Mu and sigma, for a caller that needs more than the score.
  tournament_broker::trueskill::Rating RatingOf(
      const std::string &player) const;
  std::vector<Standing> Rank(int limit) const override;
  std::string score_label() const override { return "trueskill"; }
  bool has(const std::string &candidate_id) const override;

 private:
  const CandidateView *candidates_;  // not owned
  mutable std::mutex mutex_;
  tournament_broker::trueskill::Ranker ranker_;  // guarded by mutex_
};

}  // namespace tournament_arena

#endif  // GAME_ARENA_GAME_ARENA_STANDINGS_TRUESKILL_STANDINGS_H
