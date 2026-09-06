#ifndef GAME_ARENA_GAME_ARENA_REFEREE_GAME_REGISTRY_H
#define GAME_ARENA_GAME_ARENA_REFEREE_GAME_REGISTRY_H

// The seam between the arena and the problems it hosts: name -> GameDescriptor.
//
// Both functions are DECLARED here and defined nowhere in this repo. A referee
// or client binary is assembled as "a registry + an entry-point library"
// (:referee_main, :broker_server_main, //game_arena/client:
// random_client_main), so which games exist is decided at link time by whoever
// builds the binary. //game_arena/testgame is the reference
// implementation; the game-mcts repo supplies one for its own games.

#include <map>
#include <string>

#include "game_arena/referee/game_session.h"

namespace tournament_broker {

// Registry-wide settings from the problem's match.registry_options, applied
// once at referee startup before any game begins.
//
// Opaque key/value on purpose. What is tunable is a property of the registry
// linked into this binary -- a search-based builtin might take
// "mcts_iterations", a compiled-in opponent might take nothing at all -- and
// the arena cannot name those knobs without knowing what the problem is.
//
// Every registry must define this; ignore keys you do not recognise, and note
// that an unknown key is not an error (an older referee must still run a newer
// problem's orders).
void SetRegistryOptions(const std::map<std::string, std::string> &options);

auto GameRegistry() -> const std::map<std::string, GameDescriptor> &;

}  // namespace tournament_broker

#endif  // GAME_ARENA_GAME_ARENA_REFEREE_GAME_REGISTRY_H
