// The definition of GameRegistry() that @game_arena only declares: the whole
// link between these rules and the arena.

#include <map>
#include <string>

#include "game/connect4.h"
#include "game_arena/referee/game_registry.h"

namespace tournament_broker {

void SetRegistryOptions(
    const std::map<std::string, std::string> & /*options*/) {
  // Nothing to tune; a search-based builtin would read its strength here.
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
