#ifndef RISK_GAME_AI_CPP_TOURNAMENT_SERVER_POLICIES_H
#define RISK_GAME_AI_CPP_TOURNAMENT_SERVER_POLICIES_H

// Stock TournamentPolicy adapters shared by the broker's clients: wrap a
// proposer as a uniform-random player, or run game-generic MCTS.
//
// Deliberately free of gRPC (unlike remote_client.h, which includes this):
// defining a strategy is a game-and-search concern, so a candidate's header
// should not have to pull in the transport to say how it plays.

#include <random>

#include "game_mcts/cpp/mcts/game_traits.h"
#include "game_mcts/cpp/mcts/mcts.inl"  // picker/runner definitions (auto return types)
#include "game_mcts/cpp/mcts/tournament.h"

namespace tournament_broker {

// Uniformly random valid move drawn with the game's proposer.
template <mcts::Game G, mcts::ActionProposer<G> PROPOSER>
struct ProposerPolicy {
  PROPOSER proposer{};

  auto operator()(const G &game, std::mt19937 &gen) const
      -> mcts::tournament::PolicyDecision<G> {
    const typename G::action_t action = proposer.sample(game, gen);
    return {.action = action, .successor = game.apply_action(action)};
  }
};

// Game-generic MCTS policy (same idiom as RiskMctsPolicy in
// cpp/risk/risk_tournament.cpp): |PROPOSER| expands the tree, |ROLLOUT|
// evaluates playouts.
template <mcts::Game G, mcts::ActionProposer<G> PROPOSER, typename ROLLOUT>
struct MctsPolicy {
  int iterations = 400;
  double widening_c = 2.0;
  double widening_alpha = 0.5;
  double exploration_c = 1.0;
  PROPOSER proposer{};
  ROLLOUT rollout{};

  auto operator()(const G &game, std::mt19937 &gen) const
      -> mcts::tournament::PolicyDecision<G> {
    mcts::MctsRunner<G, PROPOSER, ROLLOUT> runner(game, proposer, rollout);
    if constexpr (mcts::ChanceGame<G>) {
      auto picker = mcts::MctsStochasticNodePicker<G>(
          gen, widening_c, widening_alpha, exploration_c);
      for (int i = 0; i < iterations; ++i) {
        runner.OneIteration(picker, gen);
      }
    } else {
      auto picker = mcts::MctsNodePicker<G>(gen, widening_c, widening_alpha,
                                            exploration_c);
      for (int i = 0; i < iterations; ++i) {
        runner.OneIteration(picker, gen);
      }
    }
    const typename G::action_t action = runner.best_action();
    return {.action = action, .successor = game.apply_action(action)};
  }
};

}  // namespace tournament_broker

#endif  // RISK_GAME_AI_CPP_TOURNAMENT_SERVER_POLICIES_H
