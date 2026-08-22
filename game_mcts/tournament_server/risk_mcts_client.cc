// Risk MCTS strategy joining a tournament broker. The whole strategy wrapper
// is the three lines in main(): build the rollout, build the policy, hand it
// to PlayRemoteGames. Everything else is flags.
//
//   bazel run //game_mcts/tournament_server:risk_mcts_client
//       -- --name=deep-bot --server=localhost:50051 --iterations=400

#include <random>
#include <string>

#include <grpcpp/grpcpp.h>

#include "absl/flags/flag.h"
#include "absl/flags/parse.h"
#include "absl/log/initialize.h"
#include "absl/log/log.h"
#include "game_mcts/cpp/risk/risk_game.h"
#include "game_mcts/cpp/risk/risk_serialization.h"
#include "game_mcts/cpp/risk/strategies/risk_proposer.h"
#include "game_mcts/cpp/risk/strategies/risk_rollout_shortcuts.h"
#include "game_mcts/tournament_server/remote_client.h"
#include "game_mcts/tournament_server/tournament_broker.grpc.pb.h"

ABSL_FLAG(std::string, server, "localhost:50051", "host:port of the broker");
ABSL_FLAG(std::string, name, "", "Player name (required)");
ABSL_FLAG(std::string, opponent, "any",
          "any | builtin:random | builtin:mcts[:iterations=N] | ...");
ABSL_FLAG(int, games, 1, "Number of games to play");
ABSL_FLAG(int, iterations, 400, "MCTS iterations per move");

auto main(int argc, char **argv) -> int {
  absl::ParseCommandLine(argc, argv);
  absl::InitializeLog();

  if (absl::GetFlag(FLAGS_name).empty()) {
    LOG(ERROR) << "Missing required --name=<player name>";
    return 1;
  }

  using game_t = risk_game::RiskState<2>;
  using proposer_t = risk_game::RiskProposer<2>;

  auto rollout = mcts::MakeShortcutRollout<game_t, proposer_t>(
      &risk_game::ResolveBattleWithExpectationInPlace<2>);
  tournament_broker::MctsPolicy<game_t, proposer_t, decltype(rollout)>
      policy{.iterations = absl::GetFlag(FLAGS_iterations)};

  auto channel = grpc::CreateChannel(absl::GetFlag(FLAGS_server),
                                     grpc::InsecureChannelCredentials());
  auto stub =
      tournament_broker::proto::TournamentBroker::NewStub(channel);

  std::mt19937 gen(std::random_device{}());
  try {
    tournament_broker::PlayRemoteGames<game_t>(
        stub.get(), absl::GetFlag(FLAGS_name), "risk2",
        absl::GetFlag(FLAGS_opponent), absl::GetFlag(FLAGS_games), policy,
        gen);
  } catch (const std::exception &e) {
    LOG(ERROR) << e.what();
    return 1;
  }
  return 0;
}
