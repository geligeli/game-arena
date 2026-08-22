#include "cpp/tournament_server/play_reactor.h"

#include <string>
#include <utility>

#include "absl/log/log.h"

namespace tournament_broker {

void PlayReactor::HelloGuard::Detach() {
  std::lock_guard lock(mu_);
  reactor_ = nullptr;
}

void PlayReactor::HelloGuard::Fire() {
  // Held across the call so OnDone() cannot free the reactor underneath it.
  std::lock_guard lock(mu_);
  if (reactor_ != nullptr) {
    reactor_->OnHelloDeadline();
  }
}

PlayReactor::PlayReactor(Matchmaker *matchmaker,
                         std::chrono::milliseconds hello_timeout)
    : matchmaker_(matchmaker),
      hello_guard_(std::make_shared<HelloGuard>(this)) {
  // A peer that opens a stream and then says nothing would otherwise hold a
  // reactor open indefinitely. Keepalive does not cover it: the connection is
  // alive, just silent.
  if (hello_timeout.count() > 0) {
    hello_alarm_.Set(std::chrono::system_clock::now() + hello_timeout,
                     [guard = hello_guard_](bool ok) {
                       if (ok) {
                         guard->Fire();
                       }
                     });
  }
  // Exactly one operation before Play() returns. Until the stream is bound,
  // gRPC parks requests in a backlog whose write slot is a *single* pointer,
  // so a second queued write would silently overwrite the first.
  StartRead(&read_msg_);
}

void PlayReactor::OnHelloDeadline() {
  if (connection() != nullptr) {
    return;  // Hello arrived first.
  }
  FinishWithoutConnection(grpc::Status(grpc::StatusCode::DEADLINE_EXCEEDED,
                                       "no hello before the deadline"));
}

void PlayReactor::SendMessage(const proto::ServerMessage *msg) {
  StartWrite(msg);
}

void PlayReactor::EndRpc(const grpc::Status &status) {
  {
    std::lock_guard lock(mu_);
    finish_issued_ = true;
  }
  Finish(status);
}

auto PlayReactor::StartReadUnlessFinishing() -> void {
  std::lock_guard lock(mu_);
  if (finish_issued_) {
    return;
  }
  // Held across StartRead so a concurrent EndRpc cannot slip its Finish
  // between the check and the read: starting a read after a finish leaves an
  // operation that never completes, so OnDone() never runs and the peer hangs.
  // The opposite interleaving is harmless -- gRPC completes an already-pending
  // read with ok == false. Safe to call under mu_ because reactions are
  // dispatched on the EventEngine, never inline from StartRead, so this cannot
  // re-enter OnReadDone on this thread.
  read_msg_.Clear();
  StartRead(&read_msg_);
}

auto PlayReactor::connection() -> std::shared_ptr<PlayerConnection> {
  std::lock_guard lock(mu_);
  return conn_;
}

void PlayReactor::FinishWithoutConnection(const grpc::Status &status) {
  {
    std::lock_guard lock(mu_);
    if (finish_issued_ || conn_ != nullptr) {
      return;
    }
    finish_issued_ = true;
  }
  Finish(status);
}

auto PlayReactor::HandleHello() -> bool {
  if (!read_msg_.has_hello()) {
    FinishWithoutConnection(grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                                         "first message must be hello"));
    return false;
  }
  const proto::Hello &hello = read_msg_.hello();
  if (hello.player_name().empty() || hello.game().empty()) {
    FinishWithoutConnection(grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                                         "hello needs player_name and game"));
    return false;
  }
  LOG(INFO) << "Player '" << hello.player_name() << "' joined game '"
            << hello.game() << "' (opponent: '"
            << (hello.opponent().empty() ? "any" : hello.opponent()) << "')";

  // The hello arrived, so the deadline no longer applies. Cancel is best
  // effort; OnHelloDeadline() re-checks for a connection and does nothing once
  // one exists.
  hello_alarm_.Cancel();

  // Publish conn_ *before* Join: the matchmaker can start a game (and write to
  // this stream) on another thread the moment it is handed the handle, and
  // OnWriteDone would then have nothing to deliver the completion to.
  std::shared_ptr<PlayerConnection> conn;
  {
    std::lock_guard lock(mu_);
    conn_ = std::make_shared<PlayerConnection>(hello.player_name(), this);
    conn = conn_;
  }

  std::string error;
  if (!matchmaker_->Join(conn, hello, &error)) {
    conn->Close(grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, error));
    return false;
  }
  return true;
}

void PlayReactor::OnReadDone(bool ok) {
  if (!ok) {
    // Client half-closed (the normal end of a game) or the stream broke.
    // Either way no further action can arrive, so stop reading.
    if (auto conn = connection(); conn != nullptr) {
      matchmaker_->Disconnect(conn);
      // Flushes anything already queued -- a GameOver still in the outbox
      // reaches the client -- and then finishes.
      conn->CloseAfterFlush();
    } else {
      FinishWithoutConnection(grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                                           "stream closed before hello"));
    }
    return;
  }

  if (connection() == nullptr) {
    if (!HandleHello()) {
      return;  // Rejected: a finish is pending, never start another read.
    }
  } else if (read_msg_.has_action()) {
    if (auto conn = connection(); conn != nullptr) {
      conn->PushAction(std::move(*read_msg_.mutable_action()->mutable_action()));
    }
  }

  StartReadUnlessFinishing();
}

void PlayReactor::OnWriteDone(bool ok) {
  if (auto conn = connection(); conn != nullptr) {
    conn->OnWriteComplete(ok);
  }
}

void PlayReactor::OnCancel() {
  if (auto conn = connection(); conn != nullptr) {
    matchmaker_->Disconnect(conn);
    conn->OnCancelled();
    return;
  }
  FinishWithoutConnection(
      grpc::Status(grpc::StatusCode::CANCELLED, "call cancelled"));
}

void PlayReactor::OnDone() {
  // Before anything else: blocks until an in-flight alarm callback has
  // returned, and stops any later one from touching this object.
  hello_alarm_.Cancel();
  hello_guard_->Detach();

  std::shared_ptr<PlayerConnection> conn;
  {
    std::lock_guard lock(mu_);
    conn = std::move(conn_);
  }
  if (conn != nullptr) {
    // Severs the raw back-pointer before this object goes away. Any game still
    // holding the connection now sees a dead handle instead of a dangling one.
    conn->DetachTransport();
  }
  delete this;
}

}  // namespace tournament_broker
