#ifndef RISK_GAME_AI_CPP_TOURNAMENT_SERVER_CANDIDATE_CANDIDATE_API_H
#define RISK_GAME_AI_CPP_TOURNAMENT_SERVER_CANDIDATE_CANDIDATE_API_H

// The contract between an arena candidate and the harness that runs it.
//
// A candidate is one header that includes this file and defines exactly one
// function:
//
//   auto MakePolicy(const candidate::Params &params) -> candidate::policy_t;
//
// Everything else -- connecting, the hello handshake, deserializing state,
// serializing the chosen action, reporting the result -- belongs to
// candidate_main.cc, which the arena compiles around the submitted header.
//
// Why a factory returning a type-erased policy, rather than a fixed strategy
// shape: policy_t is mcts::tournament::AnyPolicy, so a candidate can return
// the stock MctsPolicy with its own ActionProposer, its own rollout, or a
// hand-written search that is not MCTS at all -- without any of those types
// crossing the boundary. The harness never needs to know which.
//
// The game is chosen at compile time by the arena, via CANDIDATE_GAME_* on the
// compiling target. Risk 2-player is the default.
//
// Header-only by design: game_t and kGameName differ between a risk2 build and
// a tictactoe build, so nothing here may end up in a separately compiled
// library that some other translation unit picked a different game for. The
// only compiled part, Params, lives in candidate_params.h precisely because it
// does not depend on the game.

#include <string_view>

#include "cpp/tournament_server/candidate/candidate_params.h"
#include "cpp/tournament_server/policies.h"
#include "game_mcts/cpp/mcts/tournament.h"

#if defined(CANDIDATE_GAME_TICTACTOE)
#include "game_mcts/cpp/tictactoe/tictactoe.h"
#include "game_mcts/cpp/tictactoe/tictactoe_serialization.h"
#else
#include "cpp/risk/risk_game.h"
#include "cpp/risk/risk_serialization.h"
#include "cpp/risk/strategies/risk_proposer.h"
#include "cpp/risk/strategies/risk_rollout_shortcuts.h"
#endif

namespace candidate {

#if defined(CANDIDATE_GAME_TICTACTOE)
using game_t = tictactoe::TicTacToe;
inline constexpr std::string_view kGameName = "tictactoe";
#else
inline constexpr int kNumPlayers = 2;
using game_t = risk_game::RiskState<kNumPlayers>;
inline constexpr std::string_view kGameName = "risk2";
#endif

// Any callable of (game, gen) -> PolicyDecision<game_t> converts implicitly.
using policy_t = mcts::tournament::AnyPolicy<game_t>;

}  // namespace candidate

// Defined by the candidate's own header. Called once per process, before any
// game starts; the returned policy is then used for every game and must be
// safe to call repeatedly.
auto MakePolicy(const candidate::Params &params) -> candidate::policy_t;

#endif  // RISK_GAME_AI_CPP_TOURNAMENT_SERVER_CANDIDATE_CANDIDATE_API_H
