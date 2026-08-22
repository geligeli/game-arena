// Example tournament-broker client: plays uniformly random valid moves for
// the chosen game. Doubles as a smoke test and as a reference for writing
// clients in any language (the state/action bytes are the per-game protos).
//
//   bazel run //game_mcts/tournament_server:random_client
//       -- --name=my-bot --game=tictactoe --opponent=builtin:minimax

#include <random>
#include <string>

#include <grpcpp/grpcpp.h>

#include "absl/flags/flag.h"
#include "absl/flags/parse.h"
#include "absl/log/initialize.h"
#include "absl/log/log.h"
#include "game_mcts/tournament_server/game_registry.h"
#include "game_mcts/tournament_server/tournament_broker.grpc.pb.h"

ABSL_FLAG(std::string, server, "localhost:50051", "host:port of the broker");
ABSL_FLAG(std::string, name, "", "Player name (required)");
ABSL_FLAG(std::string, game, "tictactoe", "Game to play (risk2 | tictactoe)");
ABSL_FLAG(std::string, opponent, "any",
          "any | builtin:random | builtin:mcts | builtin:minimax | ...");
ABSL_FLAG(int, games, 1, "Number of games to play");

namespace {

auto PlayOneGame(tournament_broker::proto::TournamentBroker::Stub *stub,
                 const std::string &name, const std::string &game,
                 const std::string &opponent,
                 const tournament_broker::BuiltinFn &policy,
                 std::mt19937 &gen) -> bool {
  grpc::ClientContext context;
  auto stream = stub->Play(&context);

  tournament_broker::proto::ClientMessage hello_msg;
  auto *hello = hello_msg.mutable_hello();
  hello->set_player_name(name);
  hello->set_game(game);
  hello->set_opponent(opponent);
  if (!stream->Write(hello_msg)) {
    LOG(ERROR) << "Could not send hello";
    return false;
  }

  tournament_broker::proto::ServerMessage server_msg;
  while (stream->Read(&server_msg)) {
    if (server_msg.has_game_start()) {
      const auto &start = server_msg.game_start();
      LOG(INFO) << "Game " << start.game_id() << " started as seat "
                << start.seat() << " vs " << start.opponent_name();
    } else if (server_msg.has_your_turn()) {
      tournament_broker::proto::ClientMessage reply;
      reply.mutable_action()->set_action(
          policy(server_msg.your_turn().state(), gen));
      if (!stream->Write(reply)) {
        LOG(ERROR) << "Stream died while sending action";
        return false;
      }
    } else if (server_msg.has_game_over()) {
      const auto &over = server_msg.game_over();
      LOG(INFO) << "Game over: "
                << tournament_broker::proto::GameOver::Result_Name(
                       over.result())
                << " (reason: " << over.reason() << "), new ELO "
                << over.new_elo();
      break;  // One game per stream; half-close so the server handler exits.
    }
  }
  stream->WritesDone();
  const grpc::Status status = stream->Finish();
  if (!status.ok()) {
    LOG(ERROR) << "Play RPC failed: " << status.error_message();
    return false;
  }
  return true;
}

}  // namespace

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
  bool ok = true;
  for (int i = 0; i < absl::GetFlag(FLAGS_games) && ok; ++i) {
    ok = PlayOneGame(stub.get(), name, game, absl::GetFlag(FLAGS_opponent),
                     *policy, gen);
  }
  return ok ? 0 : 1;
}
