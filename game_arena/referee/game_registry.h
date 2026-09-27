#ifndef GAME_ARENA_GAME_ARENA_REFEREE_GAME_REGISTRY_H
#define GAME_ARENA_GAME_ARENA_REFEREE_GAME_REGISTRY_H

// Declared here, defined by the registry a binary links (see testgame/).

#include <map>
#include <string>

#include "game_arena/referee/game_session.h"

namespace tournament_broker {

// The problem's match.registry_options, once at startup. Ignore unknown keys:
// an older referee must still run a newer problem's orders.
void SetRegistryOptions(const std::map<std::string, std::string> &options);

const std::map<std::string, GameDescriptor> &GameRegistry();

}  // namespace tournament_broker

#endif  // GAME_ARENA_GAME_ARENA_REFEREE_GAME_REGISTRY_H
