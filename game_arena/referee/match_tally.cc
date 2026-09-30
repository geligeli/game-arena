#include "game_arena/referee/match_tally.h"

#include <algorithm>
#include <string>
#include <vector>

#include "game_arena/standings/game_history.h"

namespace tournament_broker {

bool AddGame(const proto::GameRecord &record, const std::string &player,
             MatchTally *tally) {
  int seat = -1;
  for (int i = 0; i < record.player_names_size(); ++i) {
    if (record.player_names(i) == player) {
      seat = i;
      break;
    }
  }
  if (seat < 0) {
    return false;
  }
  const std::vector<int> places = PlacesOf(record);
  const auto place = static_cast<std::size_t>(places[seat]);
  ++tally->games;
  if (tally->finishes.size() <= place) {
    tally->finishes.resize(place + 1);
  }
  ++tally->finishes[place];
  ++(place != 0                          ? tally->losses
     : std::ranges::count(places, 0) > 1 ? tally->draws
                                         : tally->wins);
  return true;
}

MatchTally TallyOf(const proto::MatchReport &report,
                   const std::string &player) {
  MatchTally tally;
  for (const proto::GameRecord &record : report.games()) {
    AddGame(record, player, &tally);
  }
  return tally;
}

}  // namespace tournament_broker
