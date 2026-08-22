#include "game_mcts/tournament_server/game_session.h"

#include <random>

#include "gtest/gtest.h"
#include "game_mcts/cpp/risk/risk_game.h"
#include "game_mcts/cpp/risk/risk_serialization.h"
#include "game_mcts/cpp/risk/strategies/risk_proposer.h"
#include "game_mcts/cpp/tictactoe/tictactoe.h"
#include "game_mcts/cpp/tictactoe/tictactoe.pb.h"
#include "game_mcts/cpp/tictactoe/tictactoe_serialization.h"
#include "game_mcts/tournament_server/game_registry.h"

namespace tournament_broker {
namespace {

using tictactoe::TicTacToe;

TEST(GameSessionTest, TicTacToeStateRoundTripsThroughBytes) {
  GameSessionImpl<TicTacToe> session;
  const std::string bytes = session.SerializeState();
  tictactoe::proto::TicTacToeState parsed;
  ASSERT_TRUE(parsed.ParseFromString(bytes));
  EXPECT_EQ(parsed.board_state(), 0u);
  EXPECT_EQ(parsed.current_player(), 0);
}

TEST(GameSessionTest, TicTacToeAppliesValidAction) {
  GameSessionImpl<TicTacToe> session;
  using traits = mcts::GameSerializationTraits<TicTacToe>;
  std::string error;
  ASSERT_TRUE(session.ApplySerializedAction(
      traits::ActionToProto(4).SerializeAsString(), &error));
  EXPECT_EQ(session.CurrentPlayer(), 1);
  ASSERT_EQ(session.Steps().size(), 1);
  EXPECT_EQ(session.Steps()[0].player, 0);
  // Occupied cell is rejected and leaves the state untouched.
  EXPECT_FALSE(session.ApplySerializedAction(
      traits::ActionToProto(4).SerializeAsString(), &error));
  EXPECT_FALSE(error.empty());
  EXPECT_EQ(session.CurrentPlayer(), 1);
  EXPECT_EQ(session.Steps().size(), 1);
}

TEST(GameSessionTest, TicTacToeRejectsGarbageBytes) {
  GameSessionImpl<TicTacToe> session;
  std::string error;
  EXPECT_FALSE(session.ApplySerializedAction("\xff\xff\xff", &error));
  EXPECT_FALSE(session.Outcome().has_value());
}

TEST(GameSessionTest, TicTacToeDetectsWin) {
  GameSessionImpl<TicTacToe> session;
  using traits = mcts::GameSerializationTraits<TicTacToe>;
  std::string error;
  // X plays 0,1,2 (top row); O plays 3,4.
  for (const int cell : {0, 3, 1, 4, 2}) {
    ASSERT_TRUE(session.ApplySerializedAction(
        traits::ActionToProto(cell).SerializeAsString(), &error))
        << "cell " << cell << ": " << error;
  }
  const auto outcome = session.Outcome();
  ASSERT_TRUE(outcome.has_value());
  EXPECT_FALSE(outcome->is_draw);
  EXPECT_EQ(outcome->winning_player, 0);
}

TEST(GameSessionTest, TicTacToeHasNoChanceNodes) {
  GameSessionImpl<TicTacToe> session;
  EXPECT_FALSE(session.IsChanceNode());
}

TEST(GameSessionTest, RiskResolvesChanceNodes) {
  using risk_game_t = risk_game::RiskState<2>;
  GameSessionImpl<risk_game_t> session;
  std::mt19937 gen(42);
  // Drive the initial placement phase with direct (pre-serialized) actions
  // until a chance node appears, then resolve it server-side.
  using traits = mcts::GameSerializationTraits<risk_game_t>;
  std::string error;
  int chance_resolutions = 0;
  for (int i = 0; i < 200 && !session.Outcome().has_value(); ++i) {
    if (session.IsChanceNode()) {
      session.ApplyChanceAction(gen);
      ++chance_resolutions;
      continue;
    }
    // Take the first valid move from the game's own proposer path: serialize
    // any action the referee accepts (brute-force over the proto oneofs is
    // overkill here; use the session on a legal action from validMoves-style
    // sampling via the state).
    const auto &state = session.state();
    risk_game::RiskProposer<2> proposer;
    const auto action = proposer.sample(state, gen);
    ASSERT_TRUE(session.ApplySerializedAction(
        traits::ActionToProto(action).SerializeAsString(), &error))
        << error;
  }
  EXPECT_GT(chance_resolutions, 0);
  EXPECT_GT(session.MoveCount(), 0);
}

}  // namespace

TEST(GameSessionTest, MinimaxBuiltinNeverLosesToRandom) {
  const auto &registry = GameRegistry();
  const auto &desc = registry.at("tictactoe");
  std::string error;
  const auto minimax = desc.make_builtin("minimax", &error);
  ASSERT_TRUE(minimax.has_value()) << error;
  const auto random = desc.make_builtin("random", &error);
  ASSERT_TRUE(random.has_value()) << error;

  std::mt19937 gen(123);
  for (int game_index = 0; game_index < 50; ++game_index) {
    // Random plays seat 0, minimax seat 1.
    auto session = desc.new_session();
    while (!session->Outcome().has_value()) {
      const int seat = session->CurrentPlayer();
      const BuiltinFn &policy = seat == 0 ? *random : *minimax;
      const std::string action = policy(session->SerializeState(), gen);
      ASSERT_TRUE(session->ApplySerializedAction(action, &error)) << error;
    }
    const auto outcome = session->Outcome();
    ASSERT_TRUE(outcome.has_value());
    EXPECT_TRUE(outcome->is_draw || outcome->winning_player == 1)
        << "random (seat 0) beat minimax in game " << game_index;
  }
}

}  // namespace tournament_broker
