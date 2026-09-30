#include "game_arena/standings/trueskill_standings.h"

#include <boost/json/parse.hpp>
#include <boost/json/value.hpp>
#include <string>
#include <vector>

#include "absl/log/log.h"

namespace tournament_arena {

namespace json = boost::json;
using tournament_broker::proto::GameRecord;

TrueSkillStandings::TrueSkillStandings(
    const tournament_broker::GameHistory &history,
    const CandidateView *candidates,
    tournament_broker::trueskill::Params params)
    : candidates_(candidates), ranker_(params) {
  int games = 0;
  for (const std::string &line : history.AllGames()) {
    boost::system::error_code ec;
    const json::value entry = json::parse(line, ec);
    if (ec || !entry.is_object()) {
      LOG(WARNING) << "TrueSkill: skipping unparseable game index line";
      continue;
    }
    // The index keeps exactly what RecordGame reads of a record.
    GameRecord record;
    record.add_player_names(std::string(entry.at("player0").as_string()));
    record.add_player_names(std::string(entry.at("player1").as_string()));
    record.set_result(
        static_cast<GameRecord::Result>(entry.at("result").to_number<int>()));
    record.set_winning_player(entry.at("winning_player").to_number<int>());
    RecordGame(record);
    ++games;
  }
  LOG(INFO) << "TrueSkill: replayed " << games << " stored game(s)";
}

void TrueSkillStandings::RecordGame(const GameRecord &record) {
  const int winner =
      record.result() == GameRecord::WIN ? record.winning_player() : -1;
  const std::vector<std::string> players(record.player_names().begin(),
                                         record.player_names().end());
  const std::vector<int> places = {winner == 1, winner == 0};
  std::lock_guard lock(mutex_);
  ranker_.AddGame(players, places);
}

Standing TrueSkillStandings::Get(const std::string &candidate_id) const {
  tournament_broker::trueskill::PlayerRecord player;
  {
    std::lock_guard lock(mutex_);
    player = ranker_.Get(candidate_id);
  }
  Standing standing;
  standing.candidate_id = candidate_id;
  standing.score = player.rating.Conservative();
  standing.wins = player.wins;
  standing.draws = player.draws;
  standing.losses = player.losses;
  standing.mu = player.rating.mu;
  standing.sigma = player.rating.sigma;
  return standing;
}

tournament_broker::trueskill::Rating TrueSkillStandings::RatingOf(
    const std::string &player) const {
  std::lock_guard lock(mutex_);
  return ranker_.Get(player).rating;
}

bool TrueSkillStandings::has(const std::string &candidate_id) const {
  const Standing standing = Get(candidate_id);
  return standing.wins + standing.draws + standing.losses > 0;
}

std::vector<Standing> TrueSkillStandings::Rank(int limit) const {
  return RankReady(candidates_->List(), *this, limit);
}

}  // namespace tournament_arena
