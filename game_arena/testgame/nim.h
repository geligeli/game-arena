#ifndef GAME_ARENA_GAME_ARENA_TESTGAME_NIM_H
#define GAME_ARENA_GAME_ARENA_TESTGAME_NIM_H

// Single-heap Nim, so the arena is tested end to end without a game framework.
// State "<remaining>:<player to move>" ("21:0"), action "<stones taken>" ("3").

#include <string>
#include <string_view>

#include "game_arena/referee/game_session.h"

namespace arena_testgame {

inline constexpr int kStartingStones = 21;
inline constexpr int kMaxTake = 3;

// Normal play: whoever takes the last stone wins.
class NimSession final : public tournament_broker::GameSession {
 public:
  NimSession() = default;
  explicit NimSession(int remaining, int player)
      : remaining_(remaining), player_(player) {}

  std::string SerializeState() const override;
  int CurrentPlayer() const override { return player_; }
  bool IsChanceNode() const override { return false; }
  void ApplyChanceAction(std::mt19937& gen) override;
  bool ApplySerializedAction(std::string_view bytes,
                             std::string* error) override;
  std::optional<tournament_broker::GameOutcome> Outcome() const override;
  std::string RenderLastStep() const override;

 private:
  int remaining_ = kStartingStones;
  int player_ = 0;
  int winner_ = -1;
};

// False on anything malformed: these bytes come off the wire.
bool ParseState(std::string_view bytes, int* remaining, int* player);

// "random", or "optimal" so a test can assert that the stronger side wins.
std::optional<tournament_broker::BuiltinFn> MakeBuiltin(std::string_view spec,
                                                        std::string* error);

tournament_broker::GameDescriptor Descriptor();

}  // namespace arena_testgame

#endif  // GAME_ARENA_GAME_ARENA_TESTGAME_NIM_H
