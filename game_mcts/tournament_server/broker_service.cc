#include "game_mcts/tournament_server/broker_service.h"

#include "game_mcts/tournament_server/play_reactor.h"

namespace tournament_broker {

auto BrokerService::Play(grpc::CallbackServerContext * /*context*/)
    -> grpc::ServerBidiReactor<proto::ClientMessage, proto::ServerMessage> * {
  // Owned by gRPC from here on; it deletes itself in OnDone().
  return new PlayReactor(matchmaker_, hello_timeout_);
}

}  // namespace tournament_broker
