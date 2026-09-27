#ifndef GAME_ARENA_GAME_ARENA_REFEREE_GAME_SESSION_H
#define GAME_ARENA_GAME_ARENA_REFEREE_GAME_SESSION_H

// The contract between the arena and a game: states and actions are bytes.

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

// |player| is -1 for a chance resolution.
struct RecordedStep {
  int player;
  std::string action_bytes;
  int64_t unix_ms;
};

// State bytes to action bytes; the action is validated like a remote client's.
using BuiltinFn =
    std::function<std::string(std::string_view state_bytes, std::mt19937 &gen)>;

// nullopt, with *error set, for an unknown spec.
using BuiltinFactory = std::function<std::optional<BuiltinFn>(
    std::string_view spec, std::string *error)>;

class GameSession {
 public:
  virtual ~GameSession() = default;

  virtual std::string SerializeState() const = 0;
  // The state as text for a person: what a replay shows, one per step.
  virtual std::string RenderState() const { return SerializeState(); }
  virtual int CurrentPlayer() const = 0;  // seat index
  virtual bool IsChanceNode() const = 0;
  // Precondition: IsChanceNode(). Records the step.
  virtual void ApplyChanceAction(std::mt19937 &gen) = 0;
  // On failure sets *error and leaves the state untouched.
  virtual bool ApplySerializedAction(std::string_view bytes,
                                     std::string *error) = 0;
  virtual std::optional<GameOutcome> Outcome() const = 0;

  const std::vector<RecordedStep> &Steps() const { return steps_; }
  int MoveCount() const { return static_cast<int>(steps_.size()); }

 protected:
  void RecordStep(int player, std::string action_bytes);

 private:
  std::vector<RecordedStep> steps_;
};

struct GameDescriptor {
  std::string name;  // registry key
  std::function<std::unique_ptr<GameSession>()> new_session;
  BuiltinFactory make_builtin;
};

}  // namespace tournament_broker

#endif  // GAME_ARENA_GAME_ARENA_REFEREE_GAME_SESSION_H
