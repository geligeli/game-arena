#include "game/connect4.h"

#include <random>
#include <string>

#include "game_arena/referee/game_registry.h"
#include "gtest/gtest.h"

namespace connect4 {
namespace {

auto Drop(Connect4Session *session, int column) -> bool {
  std::string error;
  const bool ok =
      session->ApplySerializedAction(std::to_string(column), &error);
  EXPECT_TRUE(ok) << "column " << column << ": " << error;
  return ok;
}

TEST(Connect4Session, StartsEmptyWithPlayerZeroToMove) {
  Connect4Session session;
  EXPECT_EQ(session.SerializeState(), std::string(kCells, '.') + ":0");
  EXPECT_EQ(session.CurrentPlayer(), 0);
  EXPECT_FALSE(session.IsChanceNode());
  EXPECT_FALSE(session.Outcome().has_value());
}

TEST(Connect4Session, DiscsStackFromTheBottom) {
  Connect4Session session;
  ASSERT_TRUE(Drop(&session, 3));
  EXPECT_EQ(session.cells()[5 * kCols + 3], 'X') << "first disc sits on row 5";
  ASSERT_TRUE(Drop(&session, 3));
  EXPECT_EQ(session.cells()[4 * kCols + 3], 'O') << "second stacks on top";
  EXPECT_EQ(session.CurrentPlayer(), 0);
}

TEST(Connect4Session, RejectsIllegalActionsWithoutChangingState) {
  Connect4Session session;
  const std::string before = session.SerializeState();
  std::string error;

  EXPECT_FALSE(session.ApplySerializedAction("-1", &error));
  EXPECT_FALSE(session.ApplySerializedAction("7", &error));
  EXPECT_FALSE(session.ApplySerializedAction("middle", &error));
  EXPECT_FALSE(error.empty());

  EXPECT_EQ(session.SerializeState(), before);
  EXPECT_EQ(session.MoveCount(), 0);
}

TEST(Connect4Session, RejectsAFullColumn) {
  Connect4Session session;
  for (int i = 0; i < kRows; ++i) {
    ASSERT_TRUE(Drop(&session, 0));
  }
  std::string error;
  EXPECT_FALSE(session.ApplySerializedAction("0", &error));
  EXPECT_NE(error.find("full"), std::string::npos) << error;
}

TEST(Connect4Session, FourInARowWins) {
  Connect4Session session;
  for (int i = 0; i < 3; ++i) {
    ASSERT_TRUE(Drop(&session, i));  // X builds along the bottom row
    ASSERT_TRUE(Drop(&session, i));  // O stacks harmlessly on top of it
  }
  ASSERT_FALSE(session.Outcome().has_value());
  ASSERT_TRUE(Drop(&session, 3));
  const auto outcome = session.Outcome();
  ASSERT_TRUE(outcome.has_value());
  EXPECT_FALSE(outcome->is_draw);
  EXPECT_EQ(outcome->winning_player, 0);
}

TEST(Connect4Session, VerticalFourWins) {
  Connect4Session session;
  for (int i = 0; i < 3; ++i) {
    ASSERT_TRUE(Drop(&session, 0));  // X
    ASSERT_TRUE(Drop(&session, 1));  // O
  }
  ASSERT_TRUE(Drop(&session, 0));  // X's fourth in column 0
  const auto outcome = session.Outcome();
  ASSERT_TRUE(outcome.has_value());
  EXPECT_EQ(outcome->winning_player, 0);
}

TEST(ParseBoard, RoundTripsAndRejectsJunk) {
  Cells cells = EmptyBoard();
  cells[0] = 'X';
  cells[41] = 'O';
  int player = 1;

  Cells back = EmptyBoard();
  int back_player = 0;
  ASSERT_TRUE(ParseBoard(SerializeBoard(cells, player), &back, &back_player));
  EXPECT_EQ(back, cells);
  EXPECT_EQ(back_player, player);

  EXPECT_FALSE(ParseBoard("", &back, &back_player));
  EXPECT_FALSE(ParseBoard("too-short:0", &back, &back_player));
  EXPECT_FALSE(
      ParseBoard(std::string(kCells, '.') + ":2", &back, &back_player));
  EXPECT_FALSE(
      ParseBoard(std::string(kCells, '?') + ":0", &back, &back_player));
}

TEST(Builtins, RandomOnlyProposesLegalColumns) {
  std::string error;
  const auto random = MakeBuiltin("random", &error);
  ASSERT_TRUE(random.has_value()) << error;

  std::mt19937 gen(7);
  Connect4Session session;
  while (!session.Outcome().has_value()) {
    const std::string action = (*random)(session.SerializeState(), gen);
    ASSERT_TRUE(session.ApplySerializedAction(action, &error))
        << "action " << action << ": " << error;
  }
}

// The reason "greedy" exists: a submission needs an opponent that is not free
// to beat.
TEST(Builtins, GreedyBeatsRandomMoreOftenThanNot) {
  std::string error;
  const auto greedy = MakeBuiltin("greedy", &error);
  const auto random = MakeBuiltin("random", &error);
  ASSERT_TRUE(greedy.has_value() && random.has_value()) << error;

  std::mt19937 gen(2024);
  int greedy_wins = 0;
  constexpr int kGames = 40;
  for (int game = 0; game < kGames; ++game) {
    Connect4Session session;
    while (!session.Outcome().has_value()) {
      const auto &policy = session.CurrentPlayer() == 0 ? *greedy : *random;
      ASSERT_TRUE(session.ApplySerializedAction(
          policy(session.SerializeState(), gen), &error))
          << error;
    }
    const auto outcome = *session.Outcome();
    if (!outcome.is_draw && outcome.winning_player == 0) {
      ++greedy_wins;
    }
  }
  EXPECT_GT(greedy_wins, kGames * 3 / 4) << "greedy won " << greedy_wins;
}

TEST(Registry, ExposesConnect4ThroughTheArenaInterface) {
  const auto &registry = tournament_broker::GameRegistry();
  ASSERT_TRUE(registry.contains("connect4"));
  const auto &descriptor = registry.at("connect4");

  const auto session = descriptor.new_session();
  ASSERT_NE(session, nullptr);
  EXPECT_EQ(session->CurrentPlayer(), 0);

  std::string error;
  EXPECT_TRUE(descriptor.make_builtin("greedy", &error).has_value()) << error;
  EXPECT_FALSE(descriptor.make_builtin("nonsense", &error).has_value());
}

}  // namespace
}  // namespace connect4
