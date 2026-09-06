// The arena's own registry: one game, no framework.
//
// This is the reference for what a downstream repo has to supply in order to
// turn the referee core into a working referee -- a definition of
// GameRegistry(), and nothing else. The game-mcts repo has the same file for
// its own games.

#include "game_arena/referee/game_registry.h"

#include <map>
#include <string>

#include "game_arena/testgame/nim.h"

namespace tournament_broker {

void SetDefaultMctsIterations(int /*iterations*/) {
  // Nim has no search-based builtin, so the referee's --mcts_iterations has
  // nothing to configure here. Part of the registry interface because the
  // referee sets it unconditionally; see the note in game_registry.h.
}

auto GameRegistry() -> const std::map<std::string, GameDescriptor> & {
  static const auto *registry = [] {
    auto *out = new std::map<std::string, GameDescriptor>();
    out->emplace(arena_testgame::Descriptor().name,
                 arena_testgame::Descriptor());
    return out;
  }();
  return *registry;
}

}  // namespace tournament_broker
