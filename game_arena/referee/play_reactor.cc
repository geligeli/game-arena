#include "game_arena/referee/play_reactor.h"

#include <string>
#include <utility>

#include "absl/log/log.h"
#include "absl/strings/str_join.h"

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

PlayReactor::PlayReactor(Matchmaker *matchmaker)
    : matchmaker_(matchmaker),
      hello_guard_(std::make_shared<HelloGuard>(this)) {
  // A silent peer would hold the reactor forever; keepalive cannot see it.
  hello_alarm_.Set(std::chrono::system_clock::now() + std::chrono::seconds(30),
                   [guard = hello_guard_](bool ok) {
                     if (ok) {
                       guard->Fire();
                     }
                   });
  // Exactly one operation before Play() returns: until the stream is bound,
  // gRPC's backlog holds one write and a second would overwrite it.
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

void PlayReactor::StartReadUnlessFinishing() {
  std::lock_guard lock(mu_);
  if (finish_issued_) {
    return;
  }
  // Held across StartRead (see finish_issued_). Reactions run on the
  // EventEngine, never inline, so this cannot re-enter OnReadDone.
  read_msg_.Clear();
  StartRead(&read_msg_);
}

std::shared_ptr<PlayerConnection> PlayReactor::connection() {
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

bool PlayReactor::HandleHello() {
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
            << absl::StrJoin(hello.opponent(), ",") << "')";

  // Best effort: OnHelloDeadline() does nothing once a connection exists.
  hello_alarm_.Cancel();

  // Publish conn_ before Join: a game may write to this stream as soon as the
  // matchmaker has the handle, and OnWriteDone needs conn_ to deliver to.
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
    // Half-closed (the normal end) or broken: no further action can arrive.
    if (auto conn = connection(); conn != nullptr) {
      matchmaker_->Disconnect(conn);
      // A GameOver still queued reaches the client before the finish.
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
      conn->PushAction(
          std::move(*read_msg_.mutable_action()->mutable_action()));
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
  // First: waits out an in-flight alarm callback and fences off later ones.
  hello_alarm_.Cancel();
  hello_guard_->Detach();

  std::shared_ptr<PlayerConnection> conn;
  {
    std::lock_guard lock(mu_);
    conn = std::move(conn_);
  }
  if (conn != nullptr) {
    // Games still holding the connection see a dead handle, not a dangling one.
    conn->DetachTransport();
  }
  delete this;
}

}  // namespace tournament_broker
