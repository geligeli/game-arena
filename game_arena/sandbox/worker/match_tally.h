#ifndef GAME_ARENA_GAME_ARENA_SANDBOX_WORKER_MATCH_TALLY_H
#define GAME_ARENA_GAME_ARENA_SANDBOX_WORKER_MATCH_TALLY_H

// The one line a refereed match reports, and how the worker reads it.
//
// Split out of build_log.h, which was holding two unrelated things: how to
// compact a bazel log, and what a win is worth. Only the first is generic
// build tooling -- games, draws and an ELO number are the arena's vocabulary,
// and a sandbox that merely runs things must not have to know them.
//
// The contract is printed by the referee (see referee/main.cc) and is
// deliberately one line of ASCII, so a harness in any language can emit it
// and a failing test stays legible.

#include <string>

namespace tournament_arena {

struct RunTally {
  int games = 0;
  int wins = 0;
  int draws = 0;
  int losses = 0;
  double elo = 0.0;
};

// Parses the "RESULT games=N wins=W draws=D losses=L elo=E" line out of
// |output|. Returns false when no such line is present, which is how a match
// that never got as far as a verdict is told from one that ended 0-0.
auto ParseResultLine(const std::string &output, RunTally *tally) -> bool;

}  // namespace tournament_arena

#endif  // GAME_ARENA_GAME_ARENA_SANDBOX_WORKER_MATCH_TALLY_H
