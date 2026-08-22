#ifndef RISK_GAME_AI_CPP_BENCHGAME_BENCH_GAME_H
#define RISK_GAME_AI_CPP_BENCHGAME_BENCH_GAME_H

// A game that is deliberately not a game: two players alternate, every action
// is legal, and nothing is ever computed.
//
// It exists so load tests measure the broker instead of the strategies. With a
// real game, a client has to parse the state and pick a legal move, and the
// server has to validate it -- at a few thousand moves a second that work
// dominates, and the load generator becomes the bottleneck before the server
// does. Here a client can replay one pre-serialized action forever, so what is
// left on the wire is exactly the broker's per-move plumbing.
//
// Games run to kPlies, long enough that a benchmark window holds a steady
// number of concurrent games rather than measuring stream churn.

#include <random>
#include <string>

#include "game_mcts/cpp/mcts/game_traits.h"

namespace benchgame {

struct BenchGame {
  using action_t = int;
  static constexpr std::size_t kNumPlayers = 2;
  // ~Never reached inside a benchmark window; games end when the harness
  // disconnects, so concurrency stays flat instead of decaying.
  static constexpr int kPlies = 1'000'000;

  int ply{0};

  auto current_player() const -> int { return ply % 2; }

  auto apply_action(action_t /*action*/) const -> BenchGame {
    return BenchGame{.ply = ply + 1};
  }

  void apply_action_in_place(action_t /*action*/) { ++ply; }

  auto sample_action(std::uniform_random_bit_generator auto& /*gen*/) const
      -> action_t {
    return 0;
  }

  auto current_state() const -> mcts::game_state_t {
    if (ply >= kPlies) {
      return mcts::draw_t{};
    }
    return mcts::ongoing_t{};
  }

  auto valid_moves() const -> mcts::VectorLegalActionSet<action_t> {
    return mcts::VectorLegalActionSet<action_t>{.actions = {0}};
  }

  // Always true: that is the whole point -- a fixed action stays legal, so the
  // load generator never has to look at the state.
  auto is_valid_action(const action_t& /*action*/,
                       std::string& /*reason*/) const -> bool {
    return true;
  }
};

static_assert(mcts::Game<BenchGame>);
static_assert(mcts::InPlaceGame<BenchGame>);

}  // namespace benchgame

#endif  // RISK_GAME_AI_CPP_BENCHGAME_BENCH_GAME_H
