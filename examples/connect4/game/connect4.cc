#include "game/connect4.h"

#include <algorithm>
#include <charconv>
#include <memory>
#include <random>
#include <string>
#include <string_view>
#include <vector>

namespace connect4 {
namespace {

auto CellAt(const Cells &cells, int row, int col) -> char {
  return cells[row * kCols + col];
}

auto LegalColumns(const Cells &cells) -> std::vector<int> {
  std::vector<int> out;
  for (int col = 0; col < kCols; ++col) {
    if (LandingRow(cells, col) >= 0) {
      out.push_back(col);
    }
  }
  return out;
}

// Drops a disc and reports the resulting board. Precondition: the column has
// room.
auto Drop(const Cells &cells, int column, char disc) -> Cells {
  Cells next = cells;
  next[LandingRow(cells, column) * kCols + column] = disc;
  return next;
}

auto WinsWith(const Cells &cells, int column, char disc) -> bool {
  if (LandingRow(cells, column) < 0) {
    return false;
  }
  return WinnerDisc(Drop(cells, column, disc)) == disc;
}

auto ParseInt(std::string_view text, int *out) -> bool {
  if (text.empty()) {
    return false;
  }
  const char *begin = text.data();
  const char *end = begin + text.size();
  const std::from_chars_result parsed = std::from_chars(begin, end, *out);
  return parsed.ec == std::errc{} && parsed.ptr == end;
}

}  // namespace

auto LandingRow(const Cells &cells, int column) -> int {
  if (column < 0 || column >= kCols) {
    return -1;
  }
  for (int row = kRows - 1; row >= 0; --row) {
    if (CellAt(cells, row, column) == '.') {
      return row;
    }
  }
  return -1;
}

auto WinnerDisc(const Cells &cells) -> char {
  static constexpr int kDirs[4][2] = {{0, 1}, {1, 0}, {1, 1}, {1, -1}};
  for (int row = 0; row < kRows; ++row) {
    for (int col = 0; col < kCols; ++col) {
      const char disc = CellAt(cells, row, col);
      if (disc == '.') {
        continue;
      }
      for (const auto &dir : kDirs) {
        int run = 0;
        for (int step = 0; step < kConnect; ++step) {
          const int r = row + dir[0] * step;
          const int c = col + dir[1] * step;
          if (r < 0 || r >= kRows || c < 0 || c >= kCols ||
              CellAt(cells, r, c) != disc) {
            break;
          }
          ++run;
        }
        if (run == kConnect) {
          return disc;
        }
      }
    }
  }
  return '.';
}

auto IsFull(const Cells &cells) -> bool {
  return std::none_of(cells.begin(), cells.end(),
                      [](char c) { return c == '.'; });
}

auto SerializeBoard(const Cells &cells, int player) -> std::string {
  return std::string(cells.begin(), cells.end()) + ":" + std::to_string(player);
}

auto ParseBoard(std::string_view bytes, Cells *cells, int *player) -> bool {
  const std::size_t colon = bytes.find(':');
  if (colon != static_cast<std::size_t>(kCells)) {
    return false;
  }
  if (!ParseInt(bytes.substr(colon + 1), player) ||
      (*player != 0 && *player != 1)) {
    return false;
  }
  for (int i = 0; i < kCells; ++i) {
    const char c = bytes[i];
    if (c != '.' && c != 'X' && c != 'O') {
      return false;
    }
    (*cells)[i] = c;
  }
  return true;
}

auto Connect4Session::SerializeState() const -> std::string {
  return SerializeBoard(cells_, player_);
}

void Connect4Session::ApplyChanceAction(std::mt19937 & /*gen*/) {
  // Unreachable: IsChanceNode() is always false. Connect Four has no dice.
}

auto Connect4Session::ApplySerializedAction(std::string_view bytes,
                                            std::string *error) -> bool {
  int column = 0;
  if (!ParseInt(bytes, &column)) {
    *error = "action must be a column number, got '" + std::string(bytes) + "'";
    return false;
  }
  if (column < 0 || column >= kCols) {
    *error = "column " + std::to_string(column) + " is off the board";
    return false;
  }
  const int row = LandingRow(cells_, column);
  if (row < 0) {
    *error = "column " + std::to_string(column) + " is full";
    return false;
  }

  RecordStep(player_, std::string(bytes));
  cells_[row * kCols + column] = DiscFor(player_);
  if (WinnerDisc(cells_) == DiscFor(player_)) {
    winner_ = player_;
  } else if (IsFull(cells_)) {
    drawn_ = true;
  } else {
    player_ = 1 - player_;
  }
  return true;
}

auto Connect4Session::Outcome() const
    -> std::optional<tournament_broker::GameOutcome> {
  if (winner_ >= 0) {
    return tournament_broker::GameOutcome{.is_draw = false,
                                          .winning_player = winner_};
  }
  if (drawn_) {
    return tournament_broker::GameOutcome{.is_draw = true};
  }
  return std::nullopt;
}

auto MakeBuiltin(std::string_view spec, std::string *error)
    -> std::optional<tournament_broker::BuiltinFn> {
  if (spec == "random") {
    return [](std::string_view state_bytes, std::mt19937 &gen) -> std::string {
      Cells cells = EmptyBoard();
      int player = 0;
      if (!ParseBoard(state_bytes, &cells, &player)) {
        return "0";  // let the referee reject it
      }
      const std::vector<int> legal = LegalColumns(cells);
      if (legal.empty()) {
        return "0";
      }
      std::uniform_int_distribution<std::size_t> pick(0, legal.size() - 1);
      return std::to_string(legal[pick(gen)]);
    };
  }
  if (spec == "greedy") {
    return [](std::string_view state_bytes, std::mt19937 &gen) -> std::string {
      Cells cells = EmptyBoard();
      int player = 0;
      if (!ParseBoard(state_bytes, &cells, &player)) {
        return "0";
      }
      const std::vector<int> legal = LegalColumns(cells);
      if (legal.empty()) {
        return "0";
      }
      const char mine = DiscFor(player);
      const char theirs = DiscFor(1 - player);
      for (const int col : legal) {  // take a win
        if (WinsWith(cells, col, mine)) {
          return std::to_string(col);
        }
      }
      for (const int col : legal) {  // else deny one
        if (WinsWith(cells, col, theirs)) {
          return std::to_string(col);
        }
      }
      // Otherwise the centre, which is worth more squares than the edges.
      std::vector<int> by_centre = legal;
      std::sort(by_centre.begin(), by_centre.end(), [](int a, int b) {
        return std::abs(a - kCols / 2) < std::abs(b - kCols / 2);
      });
      std::uniform_int_distribution<std::size_t> jitter(
          0, std::min<std::size_t>(1, by_centre.size() - 1));
      return std::to_string(by_centre[jitter(gen)]);
    };
  }
  *error = "unknown builtin '" + std::string(spec) + "' (want random|greedy)";
  return std::nullopt;
}

auto Descriptor() -> tournament_broker::GameDescriptor {
  return tournament_broker::GameDescriptor{
      .name = "connect4",
      .new_session = [] { return std::make_unique<Connect4Session>(); },
      .make_builtin = &MakeBuiltin,
  };
}

}  // namespace connect4
