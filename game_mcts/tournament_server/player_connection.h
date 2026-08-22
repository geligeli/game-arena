#ifndef RISK_GAME_AI_CPP_TOURNAMENT_SERVER_PLAYER_CONNECTION_H
#define RISK_GAME_AI_CPP_TOURNAMENT_SERVER_PLAYER_CONNECTION_H

// The ClientHandle a gRPC bidi stream is driven through, split from the
// reactor that owns the RPC.
//
// Why the split: gRPC owns the reactor and reclaims it after OnDone(), while
// the matchmaker holds a shared_ptr<ClientHandle> and may call Send() from a
// game thread at any time. Those two lifetimes are incomparable -- a client can
// rage-quit while its game runs, and a game can finish while the RPC is still
// closing -- so one object cannot serve both. PlayerConnection is shared_ptr
// owned and outlives the RPC; the reactor is reached through a raw Transport*
// that OnDone() nulls out under mu_.
//
// That single rule is the whole safety argument: every transport_ call is made
// while holding mu_, and the pointer is cleared while holding mu_ from
// OnDone(), which gRPC runs after every other reaction. A game thread in
// Send() therefore either sees a live transport or nullptr, never a dangling
// one.

#include <grpcpp/support/status.h>

#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <string>

#include "game_mcts/tournament_server/client_handle.h"
#include "game_mcts/tournament_server/tournament_broker.pb.h"

namespace tournament_broker {

// The reactor side, as seen by the connection. Kept minimal (and named apart
// from ServerBidiReactor's own StartWrite/Finish) so implementors are not
// fighting name hiding.
class Transport {
 public:
  virtual ~Transport() = default;

  // Starts one write. At most one may be in flight at a time, and |msg| must
  // stay valid and unmodified until the matching OnWriteComplete().
  virtual void SendMessage(const proto::ServerMessage *msg) = 0;

  // Ends the RPC. Called at most once.
  virtual void EndRpc(const grpc::Status &status) = 0;
};

class PlayerConnection final : public ClientHandle {
 public:
  // A healthy stream never queues more than a couple of messages: the server
  // sends at most one unanswered YourTurn per seat. Anything deeper means the
  // client stopped reading, so the queue is a liveness signal, not a buffer to
  // grow.
  static constexpr std::size_t kMaxOutbox = 8;
  static constexpr std::size_t kMaxInbox = 8;

  PlayerConnection(std::string player_name, Transport *transport);

  // --- ClientHandle ---
  auto name() const -> std::string override;
  auto Send(const proto::ServerMessage &msg) -> bool override;
  auto TryPopAction() -> std::optional<std::string> override;
  void SetObserver(std::function<void()> on_event) override;
  void MarkDisconnected() override;
  auto disconnected() const -> bool override;
  void CloseAfterFlush() override;

  // --- reactor side ---

  // Queues an action received from the client.
  void PushAction(std::string action_bytes);

  // One write finished (or failed, |ok| == false).
  void OnWriteComplete(bool ok);

  // The RPC was cancelled. Always ends up requesting a finish: gRPC's
  // Server::Shutdown() waits for every call to be finished, so this must never
  // be made conditional on a game concluding.
  void OnCancelled();

  // The RPC is over and the reactor is about to go away.
  void DetachTransport();

  // Flush, then finish with |status|. Idempotent; first status wins.
  void Close(const grpc::Status &status);

 private:
  // Starts the next write, or issues the deferred finish once drained. Must be
  // called without mu_ held.
  void Pump();

  // Runs the observer, if any. Must be called without mu_ held: the observer
  // hops to a game strand and must never run under this connection's lock.
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

#endif  // RISK_GAME_AI_CPP_TOURNAMENT_SERVER_PLAYER_CONNECTION_H
