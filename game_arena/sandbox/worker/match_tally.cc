#include "game_arena/sandbox/worker/match_tally.h"

#include <sstream>
#include <string>

#include "re2/re2.h"

namespace tournament_arena {

auto ParseResultLine(const std::string &output, RunTally *tally) -> bool {
  static const RE2 kResult(
      R"(RESULT games=(\d+) wins=(\d+) draws=(\d+) losses=(\d+) elo=([-\d.]+))");
  std::istringstream lines(output);
  std::string line;
  bool found = false;
  while (std::getline(lines, line)) {
    RunTally parsed;
    if (RE2::PartialMatch(line, kResult, &parsed.games, &parsed.wins,
                          &parsed.draws, &parsed.losses, &parsed.elo)) {
      // Last one wins: a retried run appends rather than replaces.
      *tally = parsed;
      found = true;
    }
  }
  return found;
}

}  // namespace tournament_arena
