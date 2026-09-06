#ifndef GAME_ARENA_GAME_ARENA_REFEREE_GAME_SESSION_H
#define GAME_ARENA_GAME_ARENA_REFEREE_GAME_SESSION_H

// Type-erased game engine for the tournament broker. The broker core
// (matchmaker, gRPC service, HTTP leaderboard) is game-agnostic: it only ever
// sees serialized states/actions as byte strings.
//
// This header is the whole contract between the arena and a problem: implement
// GameSession, hand back a GameDescriptor, and the broker can run it. Nothing
// here knows what a game is made of, which is what lets one arena host chess,
// a coding challenge and a benchmark. Adapters that lift an existing game
// framework onto this interface live with that framework, not here.

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <vector>

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

// One entry of the game registry: how to start a session and how to build
// built-in strategies for this game.
struct GameDescriptor {
  std::string name;  // registry key, e.g. "risk2"
  std::function<std::unique_ptr<GameSession>()> new_session;
  BuiltinFactory make_builtin;
};

}  // namespace tournament_broker

#endif  // GAME_ARENA_GAME_ARENA_REFEREE_GAME_SESSION_H
