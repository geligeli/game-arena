// Pins the candidate contract: parameter parsing, and that a candidate written
// against candidate_api.h actually compiles and plays.
//
// Built with CANDIDATE_GAME_TICTACTOE, so it also covers the non-default game
// selection. That path is otherwise only exercised by BUILD files the sandbox
// worker generates at runtime, where a compile error would surface as a
// mystifying build failure for whoever submitted the candidate.

#include "cpp/tournament_server/candidate/candidate_api.h"

#include <random>

#include "gtest/gtest.h"

namespace {

using candidate::Params;

// The candidate under test: the contract's required entry point.
auto MakeTestPolicy(const Params &params) -> candidate::policy_t {
  using game_t = candidate::game_t;
  using proposer_t = mcts::DefaultProposer<game_t>;
  using rollout_t = mcts::RandomRollout<game_t, proposer_t>;
  return tournament_broker::MctsPolicy<game_t, proposer_t, rollout_t>{
      .iterations = params.get_int("iterations", 50)};
}

TEST(CandidateParamsTest, ParsesKeysAndTrimsWhitespace) {
  const Params params = Params::Parse(" iterations = 800 , widening_c=1.5 ");
  EXPECT_EQ(params.get_int("iterations", 0), 800);
  EXPECT_DOUBLE_EQ(params.get_double("widening_c", 0.0), 1.5);
  EXPECT_EQ(params.values().size(), 2u);
}

TEST(CandidateParamsTest, EmptySpecYieldsNoValues) {
  EXPECT_TRUE(Params::Parse("").values().empty());
  EXPECT_TRUE(Params::Parse(" , , ").values().empty());
}

// A knob the candidate does not understand, or one the arena mistyped, must
// never stop it from starting -- it drops out of the tournament otherwise.
TEST(CandidateParamsTest, MalformedInputFallsBackInsteadOfThrowing) {
  const Params params = Params::Parse("stray,=novalue,iterations=abc,c=1.5x");
  EXPECT_EQ(params.get_int("iterations", 400), 400);
  EXPECT_DOUBLE_EQ(params.get_double("c", 2.0), 2.0);
  EXPECT_EQ(params.get_int("absent", 7), 7);
  EXPECT_EQ(params.get("absent", "default"), "default");
  EXPECT_FALSE(params.contains("stray"));
  // The malformed entries still parsed as *present*, just not as numbers.
  EXPECT_TRUE(params.contains("iterations"));
}

TEST(CandidateParamsTest, LastValueWinsForARepeatedKey) {
  EXPECT_EQ(Params::Parse("n=1,n=2").get_int("n", 0), 2);
}

TEST(CandidateApiTest, SelectedGameIsTicTacToe) {
  EXPECT_EQ(candidate::kGameName, "tictactoe");
}

// The contract's real promise: a type-erased policy_t built from an arbitrary
// policy type still drives a full game of legal moves.
TEST(CandidateApiTest, PolicyPlaysLegalMovesToCompletion) {
  const candidate::policy_t policy = MakeTestPolicy(Params::Parse(""));

  std::mt19937 gen(1234);
  candidate::game_t game;
  int moves = 0;
  while (!mcts::is_terminal(game.current_state()) && moves < 100) {
    const auto decision = policy(game, gen);
    std::string reason;
    ASSERT_TRUE(game.is_valid_action(decision.action, reason))
        << "illegal action at move " << moves << ": " << reason;
    game = decision.successor;
    ++moves;
  }
  EXPECT_TRUE(mcts::is_terminal(game.current_state()));
  EXPECT_LE(moves, 9);  // TicTacToe cannot run longer than the board.
}

}  // namespace
