#include "game_mcts/tournament_server/player_connection.h"

#include <utility>

#include "absl/log/log.h"

namespace tournament_broker {

PlayerConnection::PlayerConnection(std::string player_name,
                                   Transport *transport)
    : player_name_(std::move(player_name)), transport_(transport) {}

auto PlayerConnection::name() const -> std::string { return player_name_; }

auto PlayerConnection::Send(const proto::ServerMessage &msg) -> bool {
  bool accepted = false;
  bool dropped = false;
  {
    std::lock_guard lock(mu_);
    if (transport_ != nullptr && !disconnected_ && !finish_requested_) {
      if (outbox_.size() < kMaxOutbox) {
        outbox_.push_back(msg);
        accepted = true;
      } else {
        // The peer has stopped reading. It cannot act on a game it will never
        // hear about, so drop it rather than buffering without bound.
        LOG(WARNING) << "Player '" << player_name_
                     << "': send queue full, closing connection";
        disconnected_ = true;
        finish_requested_ = true;
        finish_status_ = grpc::Status(grpc::StatusCode::RESOURCE_EXHAUSTED,
                                      "client is not reading its stream");
        dropped = true;
      }
    }
  }
  Pump();
  if (dropped) {
    Notify();
  }
  return accepted;
}

auto PlayerConnection::TryPopAction() -> std::optional<std::string> {
  std::lock_guard lock(mu_);
  if (inbox_.empty()) {
    return std::nullopt;
  }
  std::string action = std::move(inbox_.front());
  inbox_.pop_front();
  return action;
}

void PlayerConnection::SetObserver(std::function<void()> on_event) {
  std::lock_guard lock(mu_);
  observer_ = std::move(on_event);
}

void PlayerConnection::MarkDisconnected() {
  {
    std::lock_guard lock(mu_);
    disconnected_ = true;
  }
  Notify();
}

auto PlayerConnection::disconnected() const -> bool {
  std::lock_guard lock(mu_);
  return disconnected_;
}

void PlayerConnection::CloseAfterFlush() { Close(grpc::Status::OK); }

void PlayerConnection::PushAction(std::string action_bytes) {
  {
    std::lock_guard lock(mu_);
    if (inbox_.size() >= kMaxInbox) {
      // A client spamming actions must not grow the server without bound; the
      // freshest action is the useful one.
      inbox_.pop_front();
    }
    inbox_.push_back(std::move(action_bytes));
  }
  Notify();
}

void PlayerConnection::OnWriteComplete(bool ok) {
  bool dropped = false;
  {
    std::lock_guard lock(mu_);
    write_in_flight_ = false;
    if (!ok) {
      // The stream is broken; nothing queued behind this will get through.
      outbox_.clear();
      finish_requested_ = true;
      if (!disconnected_) {
        disconnected_ = true;
        dropped = true;
      }
    }
  }
  Pump();
  if (dropped) {
    Notify();
  }
}

void PlayerConnection::OnCancelled() {
  {
    std::lock_guard lock(mu_);
    disconnected_ = true;
    outbox_.clear();
    if (!finish_requested_) {
      finish_requested_ = true;
      finish_status_ =
          grpc::Status(grpc::StatusCode::CANCELLED, "call cancelled");
    }
  }
  // If a write is still in flight the finish is deferred; gRPC completes that
  // write with ok == false, and OnWriteComplete() pumps it out from there.
  Pump();
  Notify();
}

void PlayerConnection::DetachTransport() {
  {
    std::lock_guard lock(mu_);
    transport_ = nullptr;
    disconnected_ = true;
    outbox_.clear();
  }
  Notify();
}

void PlayerConnection::Close(const grpc::Status &status) {
  {
    std::lock_guard lock(mu_);
    if (finish_requested_) {
      return;  // First status wins.
    }
    finish_requested_ = true;
    finish_status_ = status;
  }
  Pump();
}

void PlayerConnection::Notify() {
  std::function<void()> observer;
  {
    std::lock_guard lock(mu_);
    observer = observer_;
  }
  if (observer) {
    observer();
  }
}

void PlayerConnection::Pump() {
  Transport *finish_now = nullptr;
  grpc::Status status;
  {
    std::lock_guard lock(mu_);
    if (transport_ == nullptr || write_in_flight_) {
      return;
    }
    if (!outbox_.empty()) {
      write_msg_ = std::move(outbox_.front());
      outbox_.pop_front();
      write_in_flight_ = true;
      // Safe to call under mu_: the bidi write tag is registered with
      // can_inline = false, so the completion always lands on an EventEngine
      // thread and can never re-enter Pump() on this one. Keeping it inside
      // the lock also makes the write_msg_ handoff atomic with the
      // write_in_flight_ flip -- releasing first would leave a window where we
      // are committed to a write we have not started, and if any path then
      // failed to start it, OnWriteComplete() would never fire, the deferred
      // finish would never issue, and Server::Shutdown() would hang forever.
      transport_->SendMessage(&write_msg_);
      return;
    }
    if (finish_requested_ && !finish_issued_) {
      finish_issued_ = true;  // one-shot: exactly one caller gets here
      finish_now = transport_;
      status = finish_status_;
    }
  }
  // Hoisted out of mu_: the finish tag is registered can_inline = true, so its
  // callback may run on this thread.
  if (finish_now != nullptr) {
    finish_now->EndRpc(status);
  }
}

}  // namespace tournament_broker
