#ifndef RISK_GAME_AI_CPP_TOURNAMENT_SERVER_BROKER_SERVICE_H
#define RISK_GAME_AI_CPP_TOURNAMENT_SERVER_BROKER_SERVICE_H

#include <grpcpp/grpcpp.h>
#include <grpcpp/support/server_callback.h>

#include <chrono>

#include "game_mcts/tournament_server/matchmaker.h"
#include "game_mcts/tournament_server/tournament_broker.grpc.pb.h"

namespace tournament_broker {

// gRPC front end. Each Play stream becomes a PlayReactor driven by gRPC's
// callback API, so connections cost memory rather than a parked thread.
//
// CallbackService is a typedef for WithCallbackMethod_Play<Service>, which is
// still a grpc::Service, so registration with ServerBuilder is unchanged.
class BrokerService final : public proto::TournamentBroker::CallbackService {
 public:
  // |hello_timeout| bounds how long a stream may stay open before sending its
  // hello; <= 0 disables it.
  explicit BrokerService(Matchmaker *matchmaker,
                         std::chrono::milliseconds hello_timeout =
                             std::chrono::milliseconds(30000))
      : matchmaker_(matchmaker), hello_timeout_(hello_timeout) {}

  auto Play(grpc::CallbackServerContext *context)
      -> grpc::ServerBidiReactor<proto::ClientMessage, proto::ServerMessage>
          * override;

 private:
  Matchmaker *matchmaker_;  // not owned
  std::chrono::milliseconds hello_timeout_;
};

}  // namespace tournament_broker

#endif  // RISK_GAME_AI_CPP_TOURNAMENT_SERVER_BROKER_SERVICE_H
