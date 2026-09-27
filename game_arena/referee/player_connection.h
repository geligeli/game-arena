#ifndef GAME_ARENA_GAME_ARENA_REFEREE_PLAYER_CONNECTION_H
#define GAME_ARENA_GAME_ARENA_REFEREE_PLAYER_CONNECTION_H

// ClientHandle over a gRPC stream. It outlives the reactor gRPC owns, so
// transport_ is only called, and (from OnDone()) cleared, under mu_.

#include <grpcpp/support/status.h>

#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <string>

#include "game_arena/proto/tournament_broker.pb.h"
#include "game_arena/referee/client_handle.h"

namespace tournament_broker {

// Named apart from ServerBidiReactor's StartWrite/Finish to avoid name hiding.
class Transport {
 public:
  virtual ~Transport() = default;

  // At most one in flight; |msg| must outlive the matching OnWriteComplete().
  virtual void SendMessage(const proto::ServerMessage *msg) = 0;

  // Called at most once.
  virtual void EndRpc(const grpc::Status &status) = 0;
};

class PlayerConnection final : public ClientHandle {
 public:
  // A healthy stream queues one or two; more means the client stopped reading.
  static constexpr std::size_t kMaxOutbox = 8;
  static constexpr std::size_t kMaxInbox = 8;

  PlayerConnection(std::string player_name, Transport *transport);

  std::string name() const override;
  bool Send(const proto::ServerMessage &msg) override;
  std::optional<std::string> TryPopAction() override;
  void SetObserver(std::function<void()> on_event) override;
  void MarkDisconnected() override;
  bool disconnected() const override;
  void CloseAfterFlush() override;

  void PushAction(std::string action_bytes);
  void OnWriteComplete(bool ok);
  // Always finishes: Server::Shutdown() waits for every call to be finished.
  void OnCancelled();
  void DetachTransport();
  // Flush, then finish with |status|. Idempotent; first status wins.
  void Close(const grpc::Status &status);

 private:
  // Starts the next write or the deferred finish. Call without mu_ held.
  void Pump();

  // Call without mu_ held: the observer must never run under this lock.
  void Notify();

  const std::string player_name_;

  mutable std::mutex mu_;
  Transport *transport_;  // not owned; nulled by DetachTransport()
  std::function<void()> observer_;
  std::deque<proto::ServerMessage> outbox_;
  proto::ServerMessage write_msg_;  // buffer backing the in-flight write
  std::deque<std::string> inbox_;
  bool write_in_flight_ = false;
  bool finish_requested_ = false;
  bool finish_issued_ = false;
  grpc::Status finish_status_ = grpc::Status::OK;
  bool disconnected_ = false;
};

}  // namespace tournament_broker

#endif  // GAME_ARENA_GAME_ARENA_REFEREE_PLAYER_CONNECTION_H
