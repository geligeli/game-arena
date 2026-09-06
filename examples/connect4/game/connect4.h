#ifndef CONNECT4_GAME_CONNECT4_H
#define CONNECT4_GAME_CONNECT4_H

// Connect Four as an arena game.
//
// The entire contract with the arena is tournament_broker::GameSession: turn a
// state into bytes, say whose turn it is, validate and apply an action, report
// an outcome. Nothing here knows about gRPC, sandboxes or leaderboards, and the
// arena knows nothing about columns and discs.
//
// Wire format, deliberately human-readable so a failing test is legible:
//   state  42 cells row-major from the top, then ":<player to move>"
//          e.g. "..........................................:0"
//          cells are '.', 'X' (player 0) or 'O' (player 1)
//   action the column to drop into, "0".."6"

#include <array>
#include <optional>
#include <string>
#include <string_view>

#include "game_arena/referee/game_session.h"

namespace connect4 {

inline constexpr int kCols = 7;
inline constexpr int kRows = 6;
inline constexpr int kCells = kCols * kRows;
inline constexpr int kConnect = 4;

using Cells = std::array<char, kCells>;

inline auto EmptyBoard() -> Cells {
  Cells cells;
  cells.fill('.');
  return cells;
}

inline auto DiscFor(int player) -> char { return player == 0 ? 'X' : 'O'; }

// Row 0 is the top; a disc dropped in a column lands in the highest-numbered
// free row. Returns -1 when the column is full.
auto LandingRow(const Cells &cells, int column) -> int;

// The winner's disc, or '.' if nobody has four in a row yet.
auto WinnerDisc(const Cells &cells) -> char;

auto IsFull(const Cells &cells) -> bool;

auto SerializeBoard(const Cells &cells, int player) -> std::string;
auto ParseBoard(std::string_view bytes, Cells *cells, int *player) -> bool;

class Connect4Session final : public tournament_broker::GameSession {
 public:
  Connect4Session() = default;
  Connect4Session(Cells cells, int player) : cells_(cells), player_(player) {}

  auto SerializeState() const -> std::string override;
  auto CurrentPlayer() const -> int override { return player_; }
  auto IsChanceNode() const -> bool override { return false; }
  void ApplyChanceAction(std::mt19937 &gen) override;
  auto ApplySerializedAction(std::string_view bytes,
                             std::string *error) -> bool override;
  auto Outcome() const
      -> std::optional<tournament_broker::GameOutcome> override;

  auto cells() const -> const Cells & { return cells_; }

 private:
  Cells cells_ = EmptyBoard();
  int player_ = 0;
  int winner_ = -1;
  bool drawn_ = false;
};

// "random" plays a uniform legal column; "greedy" takes a win, blocks a loss,
// and otherwise prefers the centre. "greedy" exists so a submission has a
// non-trivial opponent to be placed against.
auto MakeBuiltin(std::string_view spec, std::string *error)
    -> std::optional<tournament_broker::BuiltinFn>;

auto Descriptor() -> tournament_broker::GameDescriptor;

}  // namespace connect4

#endif  // CONNECT4_GAME_CONNECT4_H
