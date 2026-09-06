#ifndef GAME_MCTS_GAME_MCTS_TOURNAMENT_SERVER_REFEREE_GAME_REGISTRY_H
#define GAME_MCTS_GAME_MCTS_TOURNAMENT_SERVER_REFEREE_GAME_REGISTRY_H

// The seam between the arena and the problems it hosts: name -> GameDescriptor.
//
// Both functions are DECLARED here and defined nowhere in this repo. A referee
// or client binary is assembled as "a registry + an entry-point library"
// (:referee_main, :broker_server_main, //game_mcts/tournament_server/client:
// random_client_main), so which games exist is decided at link time by whoever
// builds the binary. //game_mcts/tournament_server/testgame is the reference
// implementation; //game_mcts/arena is the game_mcts one.

#include <map>
#include <string>

#include "game_mcts/tournament_server/referee/game_session.h"

namespace tournament_broker {

// Default strength for search-based builtins when the spec does not carry
// iterations=...; set from the referee's --mcts_iterations at startup. A
// registry whose builtins do not search implements this as a no-op.
// TODO: generalise to an opaque per-registry options blob -- the arena should
// not name a search algorithm.
void SetDefaultMctsIterations(int iterations);

auto GameRegistry() -> const std::map<std::string, GameDescriptor> &;

}  // namespace tournament_broker

#endif  // GAME_MCTS_GAME_MCTS_TOURNAMENT_SERVER_REFEREE_GAME_REGISTRY_H
