#ifndef GAME_ARENA_GAME_ARENA_REFEREE_BROKER_SERVICE_H
#define GAME_ARENA_GAME_ARENA_REFEREE_BROKER_SERVICE_H

#include <grpcpp/grpcpp.h>
#include <grpcpp/support/server_callback.h>

#include "game_arena/proto/tournament_broker.grpc.pb.h"
#include "game_arena/referee/matchmaker.h"

namespace tournament_broker {

// gRPC front end. Each Play stream becomes a PlayReactor driven by gRPC's
// callback API, so connections cost memory rather than a parked thread.
//
// CallbackService is a typedef for WithCallbackMethod_Play<Service>, which is
// still a grpc::Service, so registration with ServerBuilder is unchanged.
class BrokerService final : public proto::TournamentBroker::CallbackService {
 public:
  explicit BrokerService(Matchmaker *matchmaker) : matchmaker_(matchmaker) {}

  grpc::ServerBidiReactor<proto::ClientMessage, proto::ServerMessage> *Play(
      grpc::CallbackServerContext *context) override;

 private:
  Matchmaker *matchmaker_;  // not owned
};

}  // namespace tournament_broker

#endif  // GAME_ARENA_GAME_ARENA_REFEREE_BROKER_SERVICE_H
