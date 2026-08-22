#ifndef RISK_GAME_AI_CPP_TOURNAMENT_SERVER_PLAY_REACTOR_H
#define RISK_GAME_AI_CPP_TOURNAMENT_SERVER_PLAY_REACTOR_H

// Callback-API reactor for one TournamentBroker.Play stream.
//
// Replaces the synchronous handler that parked an OS thread in stream->Read()
// for the whole game: reads and writes are now completions on gRPC's
// EventEngine, so a connected (or merely queued) player costs memory instead
// of a thread.
//
// Owned by gRPC, deleted by itself in OnDone(). All player-visible state lives
// in the shared_ptr-owned PlayerConnection, which may outlive this object.

#include <grpcpp/alarm.h>
#include <grpcpp/grpcpp.h>
#include <grpcpp/support/server_callback.h>

#include <chrono>
#include <memory>
#include <mutex>

#include "cpp/tournament_server/matchmaker.h"
#include "cpp/tournament_server/player_connection.h"
#include "cpp/tournament_server/tournament_broker.grpc.pb.h"

namespace tournament_broker {

class PlayReactor final : public grpc::ServerBidiReactor<proto::ClientMessage,
                                                         proto::ServerMessage>,
                          public Transport {
 public:
  PlayReactor(Matchmaker *matchmaker, std::chrono::milliseconds hello_timeout);

  // --- ServerBidiReactor ---
  void OnReadDone(bool ok) override;
  void OnWriteDone(bool ok) override;
  void OnCancel() override;
  void OnDone() override;

  // --- Transport ---
  void SendMessage(const proto::ServerMessage *msg) override;
  void EndRpc(const grpc::Status &status) override;

 private:
  // Handles the mandatory opening hello. Returns false when the stream was
  // rejected (a finish has been requested and no further read should start).
  auto HandleHello() -> bool;

  auto connection() -> std::shared_ptr<PlayerConnection>;

  // Finishes a stream that never got as far as having a connection. Once
  // conn_ exists every finish goes through PlayerConnection instead, so the
  // "exactly once" guarantee has a single owner.
  void FinishWithoutConnection(const grpc::Status &status);

  // Starts the next read unless the RPC is already finishing.
  auto StartReadUnlessFinishing() -> void;

  void OnHelloDeadline();

  // Keeps the hello alarm from reaching a reactor gRPC has already reclaimed.
  // The alarm is not a gRPC operation, so nothing makes OnDone() wait for it;
  // same shape as PlayerConnection's Transport pointer, and cleared the same
  // way -- under the mutex, from OnDone().
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
  // Set before Finish() is called, and checked under the same lock that starts
  // a read. Starting a read after a finish is invalid and wedges the RPC: the
  // operation never completes, so OnDone() never runs and the peer waits
  // forever. The reverse order is fine -- gRPC completes an already-pending
  // read with ok == false.
  bool finish_issued_ = false;

  // One-shot, so grpc::Alarm is safe here: the "cannot re-arm while armed"
  // CHECK only bites callers that reuse an Alarm across deadlines.
  std::shared_ptr<HelloGuard> hello_guard_;
  grpc::Alarm hello_alarm_;

  proto::ClientMessage read_msg_;
};

}  // namespace tournament_broker

#endif  // RISK_GAME_AI_CPP_TOURNAMENT_SERVER_PLAY_REACTOR_H
