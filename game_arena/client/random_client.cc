// Example tournament-broker client: plays uniformly random valid moves for
// the chosen game. Doubles as a smoke test and as a reference for writing
// clients in any language (the state/action bytes are the per-game protos).
//
// Game-agnostic: it asks the linked registry for the game's "random" builtin
// and never looks at the bytes itself. Which games it can play is decided by
// which registry the binary links, so this file is a library and the binaries
// live next to their registries.
//
//   bazel run //game_arena/testgame:random_client
//       -- --name=my-bot --game=nim --opponent=builtin:optimal

#include <grpcpp/grpcpp.h>

#include <random>
#include <string>

#include "absl/flags/flag.h"
#include "absl/flags/parse.h"
#include "absl/log/initialize.h"
#include "absl/log/log.h"
#include "game_arena/client/play_loop.h"
#include "game_arena/proto/tournament_broker.grpc.pb.h"
#include "game_arena/referee/game_registry.h"

ABSL_FLAG(std::string, server, "localhost:50051", "host:port of the broker");
ABSL_FLAG(std::string, name, "", "Player name (required)");
ABSL_FLAG(std::string, game, "", "Registry key of the game to play (required)");
ABSL_FLAG(std::string, opponent, "any",
          "any | builtin:random | builtin:mcts | builtin:minimax | ...");
ABSL_FLAG(int, games, 1, "Number of games to play");

auto main(int argc, char **argv) -> int {
  absl::ParseCommandLine(argc, argv);
  absl::InitializeLog();

  const std::string name = absl::GetFlag(FLAGS_name);
  if (name.empty()) {
    LOG(ERROR) << "Missing required --name=<player name>";
    return 1;
  }
  const std::string game = absl::GetFlag(FLAGS_game);

  const auto &registry = tournament_broker::GameRegistry();
  const auto it = registry.find(game);
  if (it == registry.end()) {
    LOG(ERROR) << "Unknown game '" << game << "'";
    return 1;
  }
  std::string error;
  const auto policy = it->second.make_builtin("random", &error);
  if (!policy.has_value()) {
    LOG(ERROR) << "No random policy for game '" << game << "': " << error;
    return 1;
  }

  auto channel = grpc::CreateChannel(absl::GetFlag(FLAGS_server),
                                     grpc::InsecureChannelCredentials());
  auto stub = tournament_broker::proto::TournamentBroker::NewStub(channel);

  std::mt19937 gen(std::random_device{}());
  return tournament_client::PlayGames(stub.get(), name, game,
                                      absl::GetFlag(FLAGS_opponent),
                                      absl::GetFlag(FLAGS_games), *policy, gen)
             ? 0
             : 1;
}
