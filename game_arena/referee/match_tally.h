#ifndef GAME_ARENA_GAME_ARENA_REFEREE_MATCH_TALLY_H
#define GAME_ARENA_GAME_ARENA_REFEREE_MATCH_TALLY_H

// A match's result from one player's side, counted from its game records.

#include <span>
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

// "1st" for place 0, "2nd" for 1, ...
std::string Ordinal(int place);

// A match record for a person: W/D/L with two seats; with more, the games in
// each place, as "1st/2nd/3rd 4/2/1". |finishes| is those counts, first first.
std::string RecordText(int players, int wins, int draws, int losses,
                       std::span<const int> finishes);

}  // namespace tournament_broker

#endif  // GAME_ARENA_GAME_ARENA_REFEREE_MATCH_TALLY_H
