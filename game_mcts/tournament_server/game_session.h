#ifndef RISK_GAME_AI_CPP_TOURNAMENT_SERVER_GAME_SESSION_H
#define RISK_GAME_AI_CPP_TOURNAMENT_SERVER_GAME_SESSION_H

// Type-erased game engine for the tournament broker. The broker core
// (matchmaker, gRPC service, HTTP leaderboard) is game-agnostic: it only ever
// sees serialized states/actions as byte strings. One templated
// GameSessionImpl adapts any mcts::SerializableGame to the interface.

#include <functional>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/log/check.h"
#include "game_mcts/cpp/mcts/game_traits.h"
#include "game_mcts/cpp/mcts/serialization.h"

namespace tournament_broker {

// Outcome of a finished game; std::nullopt from Outcome() means ongoing.
struct GameOutcome {
  bool is_draw = false;
  int winning_player = -1;  // meaningful iff !is_draw
};

// One applied step of a game: the deciding player (-1 for chance resolutions)
// and the serialized action.
struct RecordedStep {
  int player;
  std::string action_bytes;
  int64_t unix_ms;
};

// A built-in strategy, type-erased the same way as remote clients: it maps a
// serialized state to a serialized action. Applying the result through
// GameSession::ApplySerializedAction gives it referee validation for free.
using BuiltinFn =
    std::function<std::string(std::string_view state_bytes, std::mt19937 &gen)>;

// Parses a builtin spec ("random", "mcts:iterations=400", "minimax", ...).
// Returns std::nullopt and sets *error on an unknown spec.
using BuiltinFactory = std::function<std::optional<BuiltinFn>(
    std::string_view spec, std::string *error)>;

class GameSession {
 public:
  virtual ~GameSession() = default;

  virtual auto SerializeState() const -> std::string = 0;
  virtual auto CurrentPlayer() const -> int = 0;  // seat index
  virtual auto IsChanceNode() const -> bool = 0;
  // Resolves a chance node with the rules-defined distribution and records the
  // step. Precondition: IsChanceNode().
  virtual void ApplyChanceAction(std::mt19937 &gen) = 0;
  // Validates (via the game's is_valid_action referee oracle) and applies a
  // serialized action for the current player. On failure sets *error and
  // returns false without touching the state.
  virtual auto ApplySerializedAction(std::string_view bytes,
                                     std::string *error) -> bool = 0;
  virtual auto Outcome() const -> std::optional<GameOutcome> = 0;

  auto Steps() const -> const std::vector<RecordedStep> & { return steps_; }
  auto MoveCount() const -> int { return static_cast<int>(steps_.size()); }

 protected:
  void RecordStep(int player, std::string action_bytes);

 private:
  std::vector<RecordedStep> steps_;
};

template <mcts::SerializableGame G>
class GameSessionImpl final : public GameSession {
 public:
  using traits = mcts::GameSerializationTraits<G>;

  explicit GameSessionImpl(G initial = G{}) : state_(std::move(initial)) {}

  auto SerializeState() const -> std::string override {
    return traits::StateToProto(state_).SerializeAsString();
  }

  auto CurrentPlayer() const -> int override { return state_.current_player(); }

  auto IsChanceNode() const -> bool override {
    if constexpr (mcts::ChanceGame<G>) {
      return state_.is_chance_node();
    } else {
      return false;
    }
  }

  void ApplyChanceAction(std::mt19937 &gen) override {
    if constexpr (mcts::ChanceGame<G>) {
      const typename G::action_t action = state_.sample_chance_action(gen);
      RecordStep(-1, traits::ActionToProto(action).SerializeAsString());
      state_ = state_.apply_action(action);
    } else {
      CHECK(false) << "ApplyChanceAction on a game without chance nodes";
    }
  }

  auto ApplySerializedAction(std::string_view bytes,
                             std::string *error) -> bool override {
    typename traits::action_proto_t action_proto;
    if (!action_proto.ParseFromArray(bytes.data(),
                                     static_cast<int>(bytes.size()))) {
      *error = "action bytes do not parse";
      return false;
    }
    const typename G::action_t action = traits::ActionFromProto(action_proto);
    if (!state_.is_valid_action(action, *error)) {
      return false;
    }
    RecordStep(state_.current_player(), std::string(bytes));
    state_ = state_.apply_action(action);
    return true;
  }

  auto Outcome() const -> std::optional<GameOutcome> override {
    const mcts::game_state_t state = state_.current_state();
    if (const auto *win = std::get_if<mcts::win_t>(&state)) {
      return GameOutcome{.is_draw = false,
                         .winning_player = win->winning_player};
    }
    if (std::get_if<mcts::draw_t>(&state) != nullptr) {
      return GameOutcome{.is_draw = true};
    }
    return std::nullopt;
  }

  // Typed access for game-specific code (tests, registry setup).
  auto state() const -> const G & { return state_; }

 private:
  G state_;
};

// One entry of the game registry: how to start a session and how to build
// built-in strategies for this game.
struct GameDescriptor {
  std::string name;  // registry key, e.g. "risk2"
  std::function<std::unique_ptr<GameSession>()> new_session;
  BuiltinFactory make_builtin;
};

}  // namespace tournament_broker

#endif  // RISK_GAME_AI_CPP_TOURNAMENT_SERVER_GAME_SESSION_H
