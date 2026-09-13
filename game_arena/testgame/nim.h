#ifndef GAME_ARENA_GAME_ARENA_TESTGAME_NIM_H
#define GAME_ARENA_GAME_ARENA_TESTGAME_NIM_H

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

#include "game_arena/referee/game_session.h"

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

  std::string SerializeState() const override;
  int CurrentPlayer() const override { return player_; }
  bool IsChanceNode() const override { return false; }
  void ApplyChanceAction(std::mt19937 &gen) override;
  bool ApplySerializedAction(std::string_view bytes,
                             std::string *error) override;
  std::optional<tournament_broker::GameOutcome> Outcome() const override;

  int remaining() const { return remaining_; }

 private:
  int remaining_ = kStartingStones;
  int player_ = 0;
  int winner_ = -1;
};

// Parses a state string as written by NimSession::SerializeState. Returns false
// on anything malformed rather than throwing: these bytes come off the wire.
bool ParseState(std::string_view bytes, int *remaining, int *player);

// Builtins for Nim. "random" plays uniformly among the legal takes; "optimal"
// plays the winning strategy (leave a multiple of four behind) and is there so
// a test can assert that a stronger opponent actually wins.
std::optional<tournament_broker::BuiltinFn> MakeBuiltin(std::string_view spec, std::string *error);

tournament_broker::GameDescriptor Descriptor();

}  // namespace arena_testgame

#endif  // GAME_ARENA_GAME_ARENA_TESTGAME_NIM_H
