#ifndef CONNECT4_BOTS_REFERENCE_STRATEGY_H
#define CONNECT4_BOTS_REFERENCE_STRATEGY_H

// The starting point for a submission. Copy this directory, rename it, change
// ChooseColumn, submit.
//
// This one takes a win if it has one, denies the opponent's if they do, and
// otherwise plays toward the centre. It beats builtin:random comfortably and
// loses to anything that searches -- which is the interesting part.

#include <algorithm>
#include <random>
#include <vector>

#include "bots/bot_api.h"

inline auto ChooseColumn(const bot::Board &board, std::mt19937 &gen) -> int {
  const std::vector<int> legal = board.LegalColumns();
  if (legal.empty()) {
    return 0;  // never asked in this state; the referee would reject it anyway
  }

  for (const int col : legal) {
    if (board.IsWinningMove(col, board.me)) {
      return col;
    }
  }
  for (const int col : legal) {
    if (board.IsWinningMove(col, board.them)) {
      return col;
    }
  }

  // Avoid handing the opponent a win on the square directly above ours.
  std::vector<int> safe;
  for (const int col : legal) {
    const connect4::Cells after = board.After(col, board.me);
    bot::Board next{
        .cells = after, .seat = board.seat, .me = board.me, .them = board.them};
    if (!next.IsWinningMove(col, board.them)) {
      safe.push_back(col);
    }
  }
  const std::vector<int> &choices = safe.empty() ? legal : safe;

  std::vector<int> by_centre = choices;
  std::sort(by_centre.begin(), by_centre.end(), [](int a, int b) {
    return std::abs(a - connect4::kCols / 2) <
           std::abs(b - connect4::kCols / 2);
  });
  std::uniform_int_distribution<std::size_t> jitter(
      0, std::min<std::size_t>(1, by_centre.size() - 1));
  return by_centre[jitter(gen)];
}

#endif  // CONNECT4_BOTS_REFERENCE_STRATEGY_H
