#ifndef GAME_ARENA_GAME_ARENA_REFEREE_PLAY_REACTOR_H
#define GAME_ARENA_GAME_ARENA_REFEREE_PLAY_REACTOR_H

// One Play stream, owned by gRPC; it deletes itself in OnDone().

#include <grpcpp/alarm.h>
#include <grpcpp/grpcpp.h>
#include <grpcpp/support/server_callback.h>

#include <chrono>
#include <memory>
#include <mutex>

#include "game_arena/proto/tournament_broker.grpc.pb.h"
#include "game_arena/referee/matchmaker.h"
#include "game_arena/referee/player_connection.h"

namespace tournament_broker {

class PlayReactor final : public grpc::ServerBidiReactor<proto::ClientMessage,
                                                         proto::ServerMessage>,
                          public Transport {
 public:
  explicit PlayReactor(Matchmaker *matchmaker);

  void OnReadDone(bool ok) override;
  void OnWriteDone(bool ok) override;
  void OnCancel() override;
  void OnDone() override;

  void SendMessage(const proto::ServerMessage *msg) override;
  void EndRpc(const grpc::Status &status) override;

 private:
  // False when the stream was rejected: a finish is pending, read no further.
  bool HandleHello();

  std::shared_ptr<PlayerConnection> connection();

  // Only before conn_ exists; after that PlayerConnection owns the one finish.
  void FinishWithoutConnection(const grpc::Status &status);

  void StartReadUnlessFinishing();

  void OnHelloDeadline();

  // OnDone() does not wait for the alarm, so it detaches this guard instead.
  class HelloGuard {
   public:
    explicit HelloGuard(PlayReactor *reactor) : reactor_(reactor) {}
    void Detach();
    void Fire();

   private:
    std::mutex mu_;
    PlayReactor *reactor_;
  };

  Matchmaker *matchmaker_;  // not owned

  std::mutex mu_;  // guards conn_ and finish_issued_
  std::shared_ptr<PlayerConnection> conn_;
  // Set before Finish(), checked under the lock that starts a read: a read
  // started after a finish never completes, so OnDone() would never run.
  bool finish_issued_ = false;

  // One-shot, so Alarm's re-arm CHECK cannot fire.
  std::shared_ptr<HelloGuard> hello_guard_;
  grpc::Alarm hello_alarm_;

  proto::ClientMessage read_msg_;
};

class BrokerService final : public proto::TournamentBroker::CallbackService {
 public:
  explicit BrokerService(Matchmaker *matchmaker) : matchmaker_(matchmaker) {}

  grpc::ServerBidiReactor<proto::ClientMessage, proto::ServerMessage> *Play(
      grpc::CallbackServerContext * /*context*/) override {
    return new PlayReactor(matchmaker_);  // gRPC owns it; OnDone() deletes it
  }

 private:
  Matchmaker *matchmaker_;  // not owned
};

}  // namespace tournament_broker

#endif  // GAME_ARENA_GAME_ARENA_REFEREE_PLAY_REACTOR_H
