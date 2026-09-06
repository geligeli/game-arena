#ifndef CONNECT4_BOTS_BOT_API_H
#define CONNECT4_BOTS_BOT_API_H

// What a submission writes.
//
// One function. Everything else -- connecting to the broker, the handshake,
// parsing the board, serializing the move, reporting the result -- is
// bot_main.cc, compiled unchanged around your header.
//
//   auto ChooseColumn(const bot::Board &board, std::mt19937 &gen) -> int;
//
// Return a column 0..6 that is not full. Returning a full or out-of-range
// column loses the game on the spot: the referee validates every action, and
// an illegal move is a loss, not a retry.

#include <random>
#include <vector>

#include "game/connect4.h"

namespace bot {

// The board from your side of it. `me` and `them` are the discs, so a strategy
// never has to care which seat it drew.
struct Board {
  connect4::Cells cells;
  int seat = 0;  // 0 or 1
  char me = 'X';
  char them = 'O';

  auto At(int row, int col) const -> char {
    return cells[row * connect4::kCols + col];
  }

  // Columns with room, in left-to-right order. Never empty unless the game is
  // over, in which case you will not be asked.
  auto LegalColumns() const -> std::vector<int> {
    std::vector<int> out;
    for (int col = 0; col < connect4::kCols; ++col) {
      if (connect4::LandingRow(cells, col) >= 0) {
        out.push_back(col);
      }
    }
    return out;
  }

  // The board as it would be after `disc` is dropped in `col`. Cheap enough to
  // call in a search.
  auto After(int col, char disc) const -> connect4::Cells {
    connect4::Cells next = cells;
    const int row = connect4::LandingRow(cells, col);
    if (row >= 0) {
      next[row * connect4::kCols + col] = disc;
    }
    return next;
  }

  // True if dropping `disc` in `col` makes four in a row.
  auto IsWinningMove(int col, char disc) const -> bool {
    return connect4::LandingRow(cells, col) >= 0 &&
           connect4::WinnerDisc(After(col, disc)) == disc;
  }
};

}  // namespace bot

#endif  // CONNECT4_BOTS_BOT_API_H
