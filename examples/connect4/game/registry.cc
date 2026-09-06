// The definition of GameRegistry() that @game_arena only declares.
//
// This file is the entire link between a problem's rules and the arena. Linking
// it into a binary alongside an arena entry point decides which games that
// binary can run -- see BUILD, where :match_referee is exactly
// ":registry + @game_arena//game_arena/referee:referee_main".

#include <map>
#include <string>

#include "game/connect4.h"
#include "game_arena/referee/game_registry.h"

namespace tournament_broker {

void SetRegistryOptions(
    const std::map<std::string, std::string> & /*options*/) {
  // Connect Four has nothing to tune. A registry with a search-based builtin
  // would read its strength here.
}

auto GameRegistry() -> const std::map<std::string, GameDescriptor> & {
  static const auto *registry = [] {
    auto *out = new std::map<std::string, GameDescriptor>();
    out->emplace(connect4::Descriptor().name, connect4::Descriptor());
    return out;
  }();
  return *registry;
}

}  // namespace tournament_broker
