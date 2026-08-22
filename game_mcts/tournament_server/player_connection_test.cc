// Unit tests for the write pump / finish sequencing that the gRPC reactor
// depends on. No gRPC here: a FakeTransport stands in for PlayReactor, which
// makes the invariants observable directly.
//
// The invariants under test are the ones gRPC enforces by crashing or hanging
// rather than by returning an error:
//   * at most one write in flight at a time (a second StartWrite corrupts the
//     in-flight op batch);
//   * Finish exactly once, and never before the last queued write drained;
//   * a cancelled call still finishes (Server::Shutdown() waits forever
//     otherwise).

#include "game_mcts/tournament_server/player_connection.h"

#include <chrono>
#include <string>
#include <vector>

#include "gtest/gtest.h"

namespace tournament_broker {
namespace {

using std::chrono::milliseconds;

// Records what the reactor would have done, and fails loudly if the
// single-write-in-flight rule is ever broken.
class FakeTransport final : public Transport {
 public:
  void SendMessage(const proto::ServerMessage *msg) override {
    EXPECT_FALSE(write_outstanding_)
        << "second StartWrite issued while one was still in flight";
    write_outstanding_ = true;
    sent_.push_back(msg->your_turn().move_number());
  }

  void EndRpc(const grpc::Status &status) override {
    ++finishes_;
    finish_status_ = status;
  }

  void ClearOutstanding() { write_outstanding_ = false; }

  auto sent() const -> const std::vector<uint64_t> & { return sent_; }
  auto finishes() const -> int { return finishes_; }
  auto finish_status() const -> const grpc::Status & { return finish_status_; }

 private:
  std::vector<uint64_t> sent_;
  bool write_outstanding_ = false;
  int finishes_ = 0;
  grpc::Status finish_status_ = grpc::Status::OK;
};

auto Turn(uint64_t move_number) -> proto::ServerMessage {
  proto::ServerMessage msg;
  msg.mutable_your_turn()->set_move_number(move_number);
  return msg;
}

// Completes the in-flight write the way gRPC would.
void CompleteWrite(FakeTransport *transport, PlayerConnection *conn, bool ok) {
  transport->ClearOutstanding();
  conn->OnWriteComplete(ok);
}

TEST(PlayerConnectionTest, WritesAreSerializedAndOrdered) {
  FakeTransport transport;
  PlayerConnection conn("alice", &transport);

  EXPECT_TRUE(conn.Send(Turn(1)));
  EXPECT_TRUE(conn.Send(Turn(2)));
  EXPECT_TRUE(conn.Send(Turn(3)));
  // Only the first reached the transport; the rest are queued behind it.
  ASSERT_EQ(transport.sent().size(), 1u);

  CompleteWrite(&transport, &conn, true);
  ASSERT_EQ(transport.sent().size(), 2u);
  CompleteWrite(&transport, &conn, true);
  ASSERT_EQ(transport.sent().size(), 3u);
  CompleteWrite(&transport, &conn, true);

  EXPECT_EQ(transport.sent(), (std::vector<uint64_t>{1, 2, 3}));
}

TEST(PlayerConnectionTest, FinishWaitsForQueuedWritesToDrain) {
  FakeTransport transport;
  PlayerConnection conn("alice", &transport);

  conn.Send(Turn(1));
  conn.Send(Turn(2));
  conn.CloseAfterFlush();
  EXPECT_EQ(transport.finishes(), 0) << "finished before draining the outbox";

  CompleteWrite(&transport, &conn, true);
  EXPECT_EQ(transport.finishes(), 0);

  CompleteWrite(&transport, &conn, true);
  // Drained: trailers now follow the last message, never precede it.
  EXPECT_EQ(transport.finishes(), 1);
  EXPECT_EQ(transport.sent(), (std::vector<uint64_t>{1, 2}));
  EXPECT_TRUE(transport.finish_status().ok());
}

TEST(PlayerConnectionTest, FinishIsIssuedExactlyOnce) {
  FakeTransport transport;
  PlayerConnection conn("alice", &transport);

  conn.CloseAfterFlush();
  conn.CloseAfterFlush();
  conn.Close(grpc::Status(grpc::StatusCode::CANCELLED, "late"));
  conn.OnCancelled();

  EXPECT_EQ(transport.finishes(), 1);
  EXPECT_TRUE(transport.finish_status().ok()) << "first status should win";
}

TEST(PlayerConnectionTest, SendAfterCloseIsRejected) {
  FakeTransport transport;
  PlayerConnection conn("alice", &transport);

  conn.CloseAfterFlush();
  EXPECT_FALSE(conn.Send(Turn(1)));
  EXPECT_TRUE(transport.sent().empty());
}

TEST(PlayerConnectionTest, OutboxOverflowDropsTheConnection) {
  FakeTransport transport;
  PlayerConnection conn("alice", &transport);

  // The first send goes straight onto the wire, so kMaxOutbox more fit behind
  // it before the queue is full.
  ASSERT_TRUE(conn.Send(Turn(0)));
  for (std::size_t i = 0; i < PlayerConnection::kMaxOutbox; ++i) {
    ASSERT_TRUE(conn.Send(Turn(i + 1))) << "queued send " << i;
  }

  EXPECT_FALSE(conn.Send(Turn(999)));
  EXPECT_TRUE(conn.disconnected())
      << "a client that stopped reading must be dropped, not buffered";
}

TEST(PlayerConnectionTest, FailedWriteDisconnectsAndFinishes) {
  FakeTransport transport;
  PlayerConnection conn("alice", &transport);

  conn.Send(Turn(1));
  conn.Send(Turn(2));
  CompleteWrite(&transport, &conn, /*ok=*/false);

  EXPECT_TRUE(conn.disconnected());
  EXPECT_EQ(transport.finishes(), 1);
  // The queued message behind the failure is dropped, not retried.
  EXPECT_EQ(transport.sent(), (std::vector<uint64_t>{1}));
}

TEST(PlayerConnectionTest, CancelAlwaysFinishesEvenMidWrite) {
  FakeTransport transport;
  PlayerConnection conn("alice", &transport);

  conn.Send(Turn(1));
  conn.OnCancelled();
  // Deferred: a write is still outstanding and only one op may be in flight.
  EXPECT_EQ(transport.finishes(), 0);

  // gRPC completes the outstanding write with ok == false after a cancel.
  CompleteWrite(&transport, &conn, /*ok=*/false);
  EXPECT_EQ(transport.finishes(), 1)
      << "Server::Shutdown() hangs forever if a cancelled call never finishes";
}

TEST(PlayerConnectionTest, DetachedConnectionNeverTouchesTheTransport) {
  FakeTransport transport;
  PlayerConnection conn("alice", &transport);

  conn.Send(Turn(1));
  conn.DetachTransport();

  EXPECT_FALSE(conn.Send(Turn(2)));
  EXPECT_TRUE(conn.disconnected());
  // Completing the in-flight write after detach must not write or finish: the
  // reactor is gone. This is the use-after-free the old raw stream pointer had.
  CompleteWrite(&transport, &conn, true);
  EXPECT_EQ(transport.sent().size(), 1u);
  EXPECT_EQ(transport.finishes(), 0);
}

TEST(PlayerConnectionTest, TryPopActionPopsInOrderThenEmpties) {
  FakeTransport transport;
  PlayerConnection conn("alice", &transport);

  conn.PushAction("first");
  conn.PushAction("second");
  EXPECT_EQ(conn.TryPopAction(), "first");
  EXPECT_EQ(conn.TryPopAction(), "second");
  EXPECT_FALSE(conn.TryPopAction().has_value());
}

// The game never blocks on the inbox: it is told to look again. An arriving
// action and a dropped connection must both produce that notification, or a
// game parks forever waiting for a turn that already resolved.
TEST(PlayerConnectionTest, ObserverFiresOnActionAndOnDisconnect) {
  FakeTransport transport;
  PlayerConnection conn("alice", &transport);

  int notifications = 0;
  conn.SetObserver([&] { ++notifications; });

  conn.PushAction("first");
  EXPECT_EQ(notifications, 1);

  conn.MarkDisconnected();
  EXPECT_EQ(notifications, 2);
  EXPECT_TRUE(conn.disconnected());
}

TEST(PlayerConnectionTest, ClearedObserverIsNotCalled) {
  FakeTransport transport;
  PlayerConnection conn("alice", &transport);

  int notifications = 0;
  conn.SetObserver([&] { ++notifications; });
  conn.SetObserver(nullptr);

  conn.PushAction("first");
  conn.MarkDisconnected();
  EXPECT_EQ(notifications, 0);
  // Clearing the observer only stops the notification; the action is still
  // queued for whoever asks next.
  EXPECT_EQ(conn.TryPopAction(), "first");
}

TEST(PlayerConnectionTest, InboxIsBounded) {
  FakeTransport transport;
  PlayerConnection conn("alice", &transport);

  // A client spamming actions must not grow the server without bound.
  const int kSpam = 4 * static_cast<int>(PlayerConnection::kMaxInbox);
  for (int i = 0; i < kSpam; ++i) {
    conn.PushAction(std::to_string(i));
  }
  int drained = 0;
  while (conn.TryPopAction().has_value()) {
    ++drained;
  }
  EXPECT_EQ(drained, static_cast<int>(PlayerConnection::kMaxInbox));
}

}  // namespace
}  // namespace tournament_broker
