#include "game_arena/client/play_loop.h"

#include <grpcpp/grpcpp.h>

#include <random>
#include <string>

#include "absl/log/log.h"

namespace tournament_client {

auto PlayOneGame(tournament_broker::proto::TournamentBroker::Stub *stub,
                 const std::string &name, const std::string &game,
                 const std::string &opponent, const ChooseActionFn &choose,
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
          choose(server_msg.your_turn().state(), gen));
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

auto PlayGames(tournament_broker::proto::TournamentBroker::Stub *stub,
               const std::string &name, const std::string &game,
               const std::string &opponent, int games,
               const ChooseActionFn &choose, std::mt19937 &gen) -> bool {
  for (int i = 0; i < games; ++i) {
    if (!PlayOneGame(stub, name, game, opponent, choose, gen)) {
      return false;
    }
  }
  return true;
}

}  // namespace tournament_client
