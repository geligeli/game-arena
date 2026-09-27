// The arena's own registry: one game, no framework.

#include "game_arena/referee/game_registry.h"

#include <map>
#include <string>

#include "game_arena/testgame/nim.h"

namespace tournament_broker {

void SetRegistryOptions(
    const std::map<std::string, std::string> & /*options*/) {
  // Nim has nothing to tune.
}

const std::map<std::string, GameDescriptor> &GameRegistry() {
  static const auto *registry = [] {
    auto *out = new std::map<std::string, GameDescriptor>();
    out->emplace(arena_testgame::Descriptor().name,
                 arena_testgame::Descriptor());
    return out;
  }();
  return *registry;
}

}  // namespace tournament_broker
