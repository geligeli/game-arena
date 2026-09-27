#include "game_arena/referee/player_connection.h"

#include <utility>

#include "absl/log/log.h"

namespace tournament_broker {

PlayerConnection::PlayerConnection(std::string player_name,
                                   Transport *transport)
    : player_name_(std::move(player_name)), transport_(transport) {}

std::string PlayerConnection::name() const { return player_name_; }

bool PlayerConnection::Send(const proto::ServerMessage &msg) {
  bool accepted = false;
  bool dropped = false;
  {
    std::lock_guard lock(mu_);
    if (transport_ != nullptr && !disconnected_ && !finish_requested_) {
      if (outbox_.size() < kMaxOutbox) {
        outbox_.push_back(msg);
        accepted = true;
      } else {
        // The peer stopped reading; drop it rather than buffer without bound.
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

std::optional<std::string> PlayerConnection::TryPopAction() {
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

bool PlayerConnection::disconnected() const {
  std::lock_guard lock(mu_);
  return disconnected_;
}

void PlayerConnection::CloseAfterFlush() { Close(grpc::Status::OK); }

void PlayerConnection::PushAction(std::string action_bytes) {
  {
    std::lock_guard lock(mu_);
    if (inbox_.size() >= kMaxInbox) {
      // Bounded; the freshest action is the useful one.
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
  // With a write in flight, OnWriteComplete(false) issues the deferred finish.
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
      // Under mu_ on purpose: the write tag is can_inline = false, so this
      // cannot re-enter Pump(), and the handoff stays atomic with the flag.
      transport_->SendMessage(&write_msg_);
      return;
    }
    if (finish_requested_ && !finish_issued_) {
      finish_issued_ = true;  // one-shot: exactly one caller gets here
      finish_now = transport_;
      status = finish_status_;
    }
  }
  // Outside mu_: the finish tag is can_inline = true and may run right here.
  if (finish_now != nullptr) {
    finish_now->EndRpc(status);
  }
}

}  // namespace tournament_broker
