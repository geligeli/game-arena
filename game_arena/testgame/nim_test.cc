#include "game_arena/testgame/nim.h"

#include <random>
#include <string>

#include "game_arena/referee/game_registry.h"
#include "gtest/gtest.h"

namespace arena_testgame {
namespace {

TEST(NimSession, StartsWithFullHeapAndPlayerZero) {
  NimSession session;
  EXPECT_EQ(session.SerializeState(), "21:0");
  EXPECT_EQ(session.CurrentPlayer(), 0);
  EXPECT_FALSE(session.IsChanceNode());
  EXPECT_FALSE(session.Outcome().has_value());
}

TEST(NimSession, ApplyingAMoveAlternatesPlayersAndRecordsTheStep) {
  NimSession session;
  std::string error;
  ASSERT_TRUE(session.ApplySerializedAction("3", &error)) << error;
  EXPECT_EQ(session.SerializeState(), "18:1");
  ASSERT_EQ(session.Steps().size(), 1u);
  EXPECT_EQ(session.Steps()[0].player, 0);
  EXPECT_EQ(session.Steps()[0].action_bytes, "3");
  EXPECT_EQ(session.MoveCount(), 1);
}

TEST(NimSession, RejectsIllegalMovesWithoutChangingState) {
  NimSession session(2, 0);
  std::string error;

  EXPECT_FALSE(session.ApplySerializedAction("0", &error));
  EXPECT_FALSE(error.empty());
  EXPECT_FALSE(session.ApplySerializedAction("4", &error));
  EXPECT_FALSE(session.ApplySerializedAction("3", &error))
      << "may not take more than remain";
  EXPECT_FALSE(session.ApplySerializedAction("two", &error));

  EXPECT_EQ(session.SerializeState(), "2:0") << "a rejected move must not move";
  EXPECT_EQ(session.MoveCount(), 0);
}

TEST(NimSession, TakingTheLastStoneWins) {
  NimSession session(2, 1);
  std::string error;
  ASSERT_TRUE(session.ApplySerializedAction("2", &error)) << error;
  const auto outcome = session.Outcome();
  ASSERT_TRUE(outcome.has_value());
  EXPECT_FALSE(outcome->is_draw);
  EXPECT_EQ(outcome->winning_player, 1);
}

TEST(ParseState, RejectsMalformedBytes) {
  int remaining = 0;
  int player = 0;
  EXPECT_FALSE(ParseState("", &remaining, &player));
  EXPECT_FALSE(ParseState("21", &remaining, &player));
  EXPECT_FALSE(ParseState("21:7", &remaining, &player));
  EXPECT_FALSE(ParseState("x:0", &remaining, &player));
  ASSERT_TRUE(ParseState("21:1", &remaining, &player));
  EXPECT_EQ(remaining, 21);
  EXPECT_EQ(player, 1);
}

TEST(Builtins, RandomOnlyEverProposesLegalMoves) {
  std::string error;
  const auto random = MakeBuiltin("random", &error);
  ASSERT_TRUE(random.has_value()) << error;

  std::mt19937 gen(12345);
  for (int remaining = 1; remaining <= kStartingStones; ++remaining) {
    NimSession session(remaining, 0);
    const std::string action = (*random)(session.SerializeState(), gen);
    EXPECT_TRUE(session.ApplySerializedAction(action, &error))
        << "remaining=" << remaining << " action=" << action << ": " << error;
  }
}

TEST(Builtins, UnknownSpecFails) {
  std::string error;
  EXPECT_FALSE(MakeBuiltin("mcts:iterations=400", &error).has_value());
  EXPECT_FALSE(error.empty());
}

// The whole point of "optimal": it must actually beat "random" from a winning
// position, which is what makes it usable as the strong side in a broker test.
TEST(Builtins, OptimalBeatsRandomFromAWinningPosition) {
  std::string error;
  const auto optimal = MakeBuiltin("optimal", &error);
  const auto random = MakeBuiltin("random", &error);
  ASSERT_TRUE(optimal.has_value() && random.has_value()) << error;

  std::mt19937 gen(999);
  for (int game = 0; game < 50; ++game) {
    NimSession session;  // 21 is a win for the player to move
    while (!session.Outcome().has_value()) {
      const auto &policy = session.CurrentPlayer() == 0 ? *optimal : *random;
      const std::string action = policy(session.SerializeState(), gen);
      ASSERT_TRUE(session.ApplySerializedAction(action, &error)) << error;
    }
    EXPECT_EQ(session.Outcome()->winning_player, 0) << "game " << game;
  }
}

TEST(Registry, ExposesNimThroughTheArenaInterface) {
  const auto &registry = tournament_broker::GameRegistry();
  ASSERT_TRUE(registry.contains("nim"));
  const tournament_broker::GameDescriptor &descriptor = registry.at("nim");
  EXPECT_EQ(descriptor.name, "nim");

  const std::unique_ptr<tournament_broker::GameSession> session =
      descriptor.new_session();
  ASSERT_NE(session, nullptr);
  EXPECT_EQ(session->SerializeState(), "21:0");

  std::string error;
  EXPECT_TRUE(descriptor.make_builtin("random", &error).has_value()) << error;

  tournament_broker::SetDefaultMctsIterations(400);  // must be a safe no-op
}

}  // namespace
}  // namespace arena_testgame
