#ifndef GAME_ARENA_GAME_ARENA_REFEREE_MATCH_TALLY_H
#define GAME_ARENA_GAME_ARENA_REFEREE_MATCH_TALLY_H

// A match's result from one player's side, counted from its game records.

#include <string>
#include <vector>

#include "game_arena/proto/tournament_broker.pb.h"

namespace tournament_broker {

// A win is a sole first place, a draw a shared one.
struct MatchTally {
  int games = 0;
  int wins = 0;
  int draws = 0;
  int losses = 0;
  std::vector<int> finishes = {};  // games per place, first first
};

// False, adding nothing, when |player| had no seat in the game.
bool AddGame(const proto::GameRecord &record, const std::string &player,
             MatchTally *tally);

MatchTally TallyOf(const proto::MatchReport &report, const std::string &player);

}  // namespace tournament_broker

#endif  // GAME_ARENA_GAME_ARENA_REFEREE_MATCH_TALLY_H
