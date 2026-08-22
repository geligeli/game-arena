#ifndef RISK_GAME_AI_CPP_TOURNAMENT_SERVER_BUILTINS_H
#define RISK_GAME_AI_CPP_TOURNAMENT_SERVER_BUILTINS_H

// Game-generic built-in strategies, exposed as BuiltinFn (serialized state ->
// serialized action). Game-specific entries (e.g. Risk MCTS) live in
// game_registry.cc next to the games' proposers/rollouts.

#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "cpp/tournament_server/game_session.h"
#include "game_mcts/cpp/mcts/game_traits.h"
#include "game_mcts/cpp/mcts/minimax.h"
#include "game_mcts/cpp/mcts/serialization.h"

namespace tournament_broker {

// Uniformly random valid move, drawn with the game's proposer.
template <mcts::SerializableGame G, typename PROPOSER>
auto RandomBuiltin(PROPOSER proposer) -> BuiltinFn {
  return [proposer](std::string_view state_bytes,
                    std::mt19937 &gen) -> std::string {
    using traits = mcts::GameSerializationTraits<G>;
    typename traits::state_proto_t state_proto;
    state_proto.ParseFromArray(state_bytes.data(),
                               static_cast<int>(state_bytes.size()));
    const G state = traits::StateFromProto(state_proto);
    const typename G::action_t action = proposer.sample(state, gen);
    return traits::ActionToProto(action).SerializeAsString();
  };
}

// Optimal play via full minimax over valid_moves(); ties broken uniformly.
// Only viable for small games (TicTacToe). Requires int actions.
template <mcts::SerializableGame G>
  requires std::same_as<typename G::action_t, int>
auto MinimaxBuiltin() -> BuiltinFn {
  return [](std::string_view state_bytes, std::mt19937 &gen) -> std::string {
    using traits = mcts::GameSerializationTraits<G>;
    typename traits::state_proto_t state_proto;
    state_proto.ParseFromArray(state_bytes.data(),
                               static_cast<int>(state_bytes.size()));
    const G state = traits::StateFromProto(state_proto);
    const int root_player = state.current_player();

    int best_value = std::numeric_limits<int>::min();
    std::vector<int> best_actions;
    for (const int action : state.valid_moves().actions) {
      const int value =
          minimax::ComputeActionValueBinaryOutcome(state, action, root_player);
      if (value > best_value) {
        best_value = value;
        best_actions = {action};
      } else if (value == best_value) {
        best_actions.push_back(action);
      }
    }
    std::uniform_int_distribution<std::size_t> pick(0, best_actions.size() - 1);
    return traits::ActionToProto(best_actions[pick(gen)]).SerializeAsString();
  };
}

}  // namespace tournament_broker

#endif  // RISK_GAME_AI_CPP_TOURNAMENT_SERVER_BUILTINS_H
