#include "game_arena/referee/broker_service.h"

#include "game_arena/referee/play_reactor.h"

namespace tournament_broker {

grpc::ServerBidiReactor<proto::ClientMessage, proto::ServerMessage>* BrokerService::Play(grpc::CallbackServerContext* /*context*/) {
  // Owned by gRPC from here on; it deletes itself in OnDone().
  return new PlayReactor(matchmaker_, hello_timeout_);
}

}  // namespace tournament_broker
