#ifndef RISK_GAME_AI_CPP_TOURNAMENT_SERVER_CANDIDATES_DEV_STRATEGY_H
#define RISK_GAME_AI_CPP_TOURNAMENT_SERVER_CANDIDATES_DEV_STRATEGY_H

// The worked example every candidate starts from, and the scratch strategy the
// dev_bot target builds. Copy this directory, edit, iterate against a live
// broker, then submit it:
//
//   bazel run //game_mcts/tournament_server/candidate:dev_bot --
//       --name=me-dev --server=localhost:50051 --opponent=builtin:mcts
//       --games=5
//
// The whole contract is: include candidate_api.h, define MakePolicy. What you
// return is up to you -- the stock MCTS with a proposer of your own (below), a
// different rollout, or a search that is not MCTS at all. Anything callable as
// policy(game, gen) -> mcts::tournament::PolicyDecision<game_t> converts to
// candidate::policy_t.

#include <random>

#include "cpp/tournament_server/candidate/candidate_api.h"

// This example is the stock Risk MCTS bot: the repo's proposer expands the
// tree, and battles in rollouts are resolved by their expected outcome rather
// than rolled out die by die.
inline auto MakePolicy(const candidate::Params &params) -> candidate::policy_t {
  using game_t = candidate::game_t;
  using proposer_t = risk_game::RiskProposer<candidate::kNumPlayers>;

  auto rollout = mcts::MakeShortcutRollout<game_t, proposer_t>(
      &risk_game::ResolveBattleWithExpectationInPlace<candidate::kNumPlayers>);

  return tournament_broker::MctsPolicy<game_t, proposer_t, decltype(rollout)>{
      .iterations = params.get_int("iterations", 400),
      .widening_c = params.get_double("widening_c", 2.0),
      .widening_alpha = params.get_double("widening_alpha", 0.5),
      .exploration_c = params.get_double("exploration_c", 1.0),
      .rollout = rollout,
  };
}

// ---------------------------------------------------------------------------
// Writing your own proposer
// ---------------------------------------------------------------------------
//
// The proposer is where most of the strategy lives: it decides which moves the
// search ever considers. See game_mcts/cpp/mcts/README.md (game-mcts repo)
// and cpp/risk/strategies/risk_proposer.h for the full contract. The shape is:
//
//   struct MyProposer {
//     using game_t = candidate::game_t;
//     using action_t = game_t::action_t;
//
//     // Every action worth searching, for tree expansion.
//     auto propose(const game_t &state) const -> SomeActionGenerator;
//
//     // One action, for rollouts. Must be fast.
//     auto sample(const game_t &state, std::mt19937 &gen) const -> action_t;
//
//     // Optional (mcts::BoundedProposer): how many actions propose() yields.
//     auto support_size(const game_t &state) const -> std::size_t;
//   };
//   static_assert(mcts::ActionProposer<MyProposer, candidate::game_t>);
//
// Two traps that cost real debugging time, both documented in-tree:
//
//  - support_size() must mirror sample()'s branches exactly, or DedupSampler
//    asserts (game_mcts/cpp/mcts/game_traits.h in the game-mcts repo).
//  - A proposer that avoids attacking stalls rollouts until the move cap, so
//    games take minutes and finish as draws (cpp/risk/tuning_result.md).
//    Always sanity-check against builtin:random before submitting.

#endif  // RISK_GAME_AI_CPP_TOURNAMENT_SERVER_CANDIDATES_DEV_STRATEGY_H
