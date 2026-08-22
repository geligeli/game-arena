#include "cpp/tournament_server/game_registry.h"

#include <string_view>

#include "game_mcts/cpp/mcts/mcts.inl"
#include "cpp/risk/risk_game.h"
#include "cpp/risk/strategies/risk_proposer.h"
#include "cpp/risk/strategies/risk_rollout_shortcuts.h"
#include "cpp/risk/risk_serialization.h"
#include "cpp/benchgame/bench_game.h"
#include "cpp/benchgame/bench_serialization.h"
#include "game_mcts/cpp/tictactoe/tictactoe.h"
#include "game_mcts/cpp/tictactoe/tictactoe_serialization.h"
#include "cpp/tournament_server/builtins.h"

namespace tournament_broker {

namespace {

int g_default_mcts_iterations = 400;

// Parses "mcts" or "mcts:iterations=N"; returns false on a malformed spec.
auto ParseMctsSpec(std::string_view spec, int *iterations, std::string *error)
    -> bool {
  *iterations = g_default_mcts_iterations;
  if (spec == "mcts") {
    return true;
  }
  constexpr std::string_view kPrefix = "mcts:iterations=";
  if (spec.substr(0, kPrefix.size()) != kPrefix) {
    *error = "unknown builtin spec '" + std::string(spec) + "'";
    return false;
  }
  try {
    *iterations = std::stoi(std::string(spec.substr(kPrefix.size())));
  } catch (const std::exception &) {
    *error = "bad iterations in builtin spec '" + std::string(spec) + "'";
    return false;
  }
  if (*iterations <= 0) {
    *error = "iterations must be positive in '" + std::string(spec) + "'";
    return false;
  }
  return true;
}

// ---------------------------------------------------------------------------
// Risk (2 players)
// ---------------------------------------------------------------------------

using risk_game_t = risk_game::RiskState<2>;
using risk_proposer_t = risk_game::RiskProposer<2>;

// MCTS with the shortcut (expected-battle) rollout, mirroring RiskMctsPolicy
// in cpp/risk/risk_tournament.cpp.
auto RiskMctsBuiltin(int iterations) -> BuiltinFn {
  return [iterations](std::string_view state_bytes,
                      std::mt19937 &gen) -> std::string {
    using traits = mcts::GameSerializationTraits<risk_game_t>;
    traits::state_proto_t state_proto;
    state_proto.ParseFromArray(state_bytes.data(),
                               static_cast<int>(state_bytes.size()));
    const risk_game_t state = traits::StateFromProto(state_proto);

    const risk_proposer_t proposer{};
    auto rollout = mcts::MakeShortcutRollout<risk_game_t, risk_proposer_t>(
        &risk_game::ResolveBattleWithExpectationInPlace<2>);
    mcts::MctsRunner<risk_game_t, risk_proposer_t, decltype(rollout)> runner(
        state, proposer, rollout);
    auto picker =
        mcts::MctsStochasticNodePicker<risk_game_t>(gen, 2.0, 0.5, 1.0);
    for (int i = 0; i < iterations; ++i) {
      runner.OneIteration(picker, gen);
    }
    return traits::ActionToProto(runner.best_action()).SerializeAsString();
  };
}

auto MakeRiskBuiltin(std::string_view spec, std::string *error)
    -> std::optional<BuiltinFn> {
  if (spec == "random") {
    return RandomBuiltin<risk_game_t>(risk_proposer_t{});
  }
  int iterations;
  if (ParseMctsSpec(spec, &iterations, error)) {
    return RiskMctsBuiltin(iterations);
  }
  if (error->empty()) {
    *error = "unknown builtin spec '" + std::string(spec) + "'";
  }
  return std::nullopt;
}

// ---------------------------------------------------------------------------
// TicTacToe
// ---------------------------------------------------------------------------

using tictactoe::TicTacToe;

auto MakeTicTacToeBuiltin(std::string_view spec, std::string *error)
    -> std::optional<BuiltinFn> {
  if (spec == "random") {
    return RandomBuiltin<TicTacToe>(mcts::DefaultProposer<TicTacToe>{});
  }
  if (spec == "minimax") {
    return MinimaxBuiltin<TicTacToe>();
  }
  *error = "unknown builtin spec '" + std::string(spec) + "'";
  return std::nullopt;
}

// ---------------------------------------------------------------------------
// Bench -- load-test filler, not a real game
// ---------------------------------------------------------------------------

using benchgame::BenchGame;

auto MakeBenchBuiltin(std::string_view spec, std::string *error)
    -> std::optional<BuiltinFn> {
  if (spec == "random") {
    return RandomBuiltin<BenchGame>(mcts::DefaultProposer<BenchGame>{});
  }
  *error = "unknown builtin spec '" + std::string(spec) + "'";
  return std::nullopt;
}

}  // namespace

void SetDefaultMctsIterations(int iterations) {
  g_default_mcts_iterations = iterations;
}

auto GameRegistry() -> const std::map<std::string, GameDescriptor> & {
  static const auto *kRegistry = new std::map<std::string, GameDescriptor>{
      {"risk2",
       GameDescriptor{
           .name = "risk2",
           .new_session =
               [] { return std::make_unique<GameSessionImpl<risk_game_t>>(); },
           .make_builtin = MakeRiskBuiltin,
       }},
      {"bench",
       GameDescriptor{
           .name = "bench",
           .new_session =
               [] { return std::make_unique<GameSessionImpl<BenchGame>>(); },
           .make_builtin = MakeBenchBuiltin,
       }},
      {"tictactoe",
       GameDescriptor{
           .name = "tictactoe",
           .new_session =
               [] { return std::make_unique<GameSessionImpl<TicTacToe>>(); },
           .make_builtin = MakeTicTacToeBuiltin,
       }},
  };
  return *kRegistry;
}

}  // namespace tournament_broker
