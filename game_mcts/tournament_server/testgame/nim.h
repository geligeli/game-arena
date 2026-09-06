#ifndef GAME_MCTS_GAME_MCTS_TOURNAMENT_SERVER_TESTGAME_NIM_H
#define GAME_MCTS_GAME_MCTS_TOURNAMENT_SERVER_TESTGAME_NIM_H

// Single-heap Nim: the arena's own game, owned by the arena.
//
// It exists so the broker, the matchmaker and the sandbox can be tested end to
// end without a game framework in the picture. That is the point: if the
// arena's tests needed one, the arena would not really be independent of it.
// Nim is small enough to read in one sitting and real enough to have a winner,
// an illegal-move path and a builtin that actually plays well.
//
// Wire format, deliberately human-readable so a failing test is legible:
//   state  "<remaining>:<player to move>"   e.g. "21:0"
//   action "<stones taken>"                 e.g. "3"

#include <string>
#include <string_view>

#include "game_mcts/tournament_server/referee/game_session.h"

namespace arena_testgame {

inline constexpr int kStartingStones = 21;
inline constexpr int kMaxTake = 3;

// Normal play: players alternate taking 1..kMaxTake stones, and whoever takes
// the last stone wins.
class NimSession final : public tournament_broker::GameSession {
 public:
  NimSession() = default;
  explicit NimSession(int remaining, int player)
      : remaining_(remaining), player_(player) {}

  auto SerializeState() const -> std::string override;
  auto CurrentPlayer() const -> int override { return player_; }
  auto IsChanceNode() const -> bool override { return false; }
  void ApplyChanceAction(std::mt19937 &gen) override;
  auto ApplySerializedAction(std::string_view bytes,
                             std::string *error) -> bool override;
  auto Outcome() const
      -> std::optional<tournament_broker::GameOutcome> override;

  auto remaining() const -> int { return remaining_; }

 private:
  int remaining_ = kStartingStones;
  int player_ = 0;
  int winner_ = -1;
};

// Parses a state string as written by NimSession::SerializeState. Returns false
// on anything malformed rather than throwing: these bytes come off the wire.
auto ParseState(std::string_view bytes, int *remaining, int *player) -> bool;

// Builtins for Nim. "random" plays uniformly among the legal takes; "optimal"
// plays the winning strategy (leave a multiple of four behind) and is there so
// a test can assert that a stronger opponent actually wins.
auto MakeBuiltin(std::string_view spec, std::string *error)
    -> std::optional<tournament_broker::BuiltinFn>;

auto Descriptor() -> tournament_broker::GameDescriptor;

}  // namespace arena_testgame

#endif  // GAME_MCTS_GAME_MCTS_TOURNAMENT_SERVER_TESTGAME_NIM_H
