#ifndef RISK_GAME_AI_CPP_TOURNAMENT_SERVER_REMOTE_CLIENT_H
#define RISK_GAME_AI_CPP_TOURNAMENT_SERVER_REMOTE_CLIENT_H

// Client-side counterpart of the broker: wraps any typed strategy
// (mcts::tournament::TournamentPolicy<G>) so it can join a tournament server.
// The wrapper owns the whole protocol — connect, hello, deserialize state,
// call the policy, serialize the action back — so joining is:
//
//   auto rollout = mcts::MakeShortcutRollout<game_t, proposer_t>(...);
//   MctsPolicy<game_t, proposer_t, decltype(rollout)> policy{.iterations=400};
//   PlayRemoteGames<game_t>(stub.get(), "my-bot", "risk2", "any", 1, policy,
//                           gen);
//
// The two stock adapters -- ProposerPolicy (uniform sample from a proposer)
// and MctsPolicy (game-generic MCTS) -- live in policies.h and are re-exported
// here, so existing clients keep getting them from this header.

#include <grpcpp/grpcpp.h>

#include <random>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "absl/log/log.h"
#include "game_mcts/tournament_server/policies.h"
#include "game_mcts/tournament_server/tournament_broker.grpc.pb.h"
#include "game_mcts/cpp/mcts/game_traits.h"
#include "game_mcts/cpp/mcts/serialization.h"
#include "game_mcts/cpp/mcts/tournament.h"

namespace tournament_broker {

struct RemoteGameResult {
  proto::GameOver::Result result;
  std::string reason;
  double new_elo;
};

// Plays |num_games| games on the broker with |policy|. Throws
// std::runtime_error on RPC failures (unknown game, dead server, ...).
// The policy is only ever consulted at decision nodes; chance nodes are
// resolved by the server.
template <mcts::SerializableGame G, typename P>
  requires mcts::tournament::TournamentPolicy<P, G>
auto PlayRemoteGames(proto::TournamentBroker::Stub *stub,
                     const std::string &player_name, const std::string &game,
                     const std::string &opponent, int num_games, P policy,
                     std::mt19937 &gen) -> std::vector<RemoteGameResult> {
  using traits = mcts::GameSerializationTraits<G>;
  std::vector<RemoteGameResult> results;

  for (int game_index = 0; game_index < num_games; ++game_index) {
    grpc::ClientContext context;
    auto stream = stub->Play(&context);

    proto::ClientMessage hello_msg;
    auto *hello = hello_msg.mutable_hello();
    hello->set_player_name(player_name);
    hello->set_game(game);
    hello->set_opponent(opponent);
    if (!stream->Write(hello_msg)) {
      throw std::runtime_error("could not send hello (server down?)");
    }

    proto::ServerMessage server_msg;
    bool got_game_over = false;
    // A failed write means the server already finished the call -- normally
    // because this strategy overran its turn timeout or game time budget. The
    // read loop has to continue regardless: Finish() blocks until the stream
    // is drained, so returning here with a GameOver still queued would hang
    // rather than report the loss.
    bool writes_failed = false;
    while (stream->Read(&server_msg)) {
      if (server_msg.has_game_start()) {
        LOG(INFO) << "Game " << server_msg.game_start().game_id() << ": seat "
                  << server_msg.game_start().seat() << " vs "
                  << server_msg.game_start().opponent_name();
      } else if (server_msg.has_your_turn() && !writes_failed) {
        typename traits::state_proto_t state_proto;
        if (!state_proto.ParseFromString(server_msg.your_turn().state())) {
          throw std::runtime_error("unparseable state from server");
        }
        const auto decision = policy(traits::StateFromProto(state_proto), gen);
        proto::ClientMessage reply;
        reply.mutable_action()->set_action(
            traits::ActionToProto(decision.action).SerializeAsString());
        writes_failed = !stream->Write(reply);
      } else if (server_msg.has_game_over()) {
        const auto &over = server_msg.game_over();
        LOG(INFO) << "Game over: "
                  << proto::GameOver::Result_Name(over.result())
                  << " (reason: " << over.reason() << "), new ELO "
                  << over.new_elo();
        results.push_back(RemoteGameResult{.result = over.result(),
                                           .reason = over.reason(),
                                           .new_elo = over.new_elo()});
        got_game_over = true;
        break;  // One game per stream; half-close so the server exits.
      }
    }
    stream->WritesDone();
    const grpc::Status status = stream->Finish();
    if (!status.ok()) {
      throw std::runtime_error("Play RPC failed: " + status.error_message());
    }
    if (!got_game_over) {
      throw std::runtime_error("stream closed without game_over");
    }
  }
  return results;
}

}  // namespace tournament_broker

#endif  // RISK_GAME_AI_CPP_TOURNAMENT_SERVER_REMOTE_CLIENT_H
