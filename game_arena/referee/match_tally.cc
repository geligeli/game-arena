#include "game_arena/referee/match_tally.h"

#include <string>

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
  ++tally->games;
  if (record.result() == proto::GameRecord::DRAW) {
    ++tally->draws;
  } else if (record.winning_player() == seat) {
    ++tally->wins;
  } else {
    ++tally->losses;
  }
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
