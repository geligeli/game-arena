#ifndef GAME_MCTS_GAME_MCTS_TOURNAMENT_SERVER_REFEREE_GAME_REGISTRY_H
#define GAME_MCTS_GAME_MCTS_TOURNAMENT_SERVER_REFEREE_GAME_REGISTRY_H

// The broker's game registry: name -> GameDescriptor. Adding a game means
// adding one entry here (it must satisfy mcts::SerializableGame).

#include <map>
#include <string>

#include "game_mcts/tournament_server/referee/game_session.h"

namespace tournament_broker {

// Default MCTS strength for "builtin:mcts" when the spec does not carry
// iterations=...; set from main's --mcts_iterations at startup.
void SetDefaultMctsIterations(int iterations);

auto GameRegistry() -> const std::map<std::string, GameDescriptor> &;

}  // namespace tournament_broker

#endif  // GAME_MCTS_GAME_MCTS_TOURNAMENT_SERVER_REFEREE_GAME_REGISTRY_H
