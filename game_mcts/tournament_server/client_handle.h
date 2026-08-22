#ifndef RISK_GAME_AI_CPP_TOURNAMENT_SERVER_CLIENT_HANDLE_H
#define RISK_GAME_AI_CPP_TOURNAMENT_SERVER_CLIENT_HANDLE_H

// Abstraction over one connected player, independent of the transport. The
// gRPC front end implements it over a bidirectional stream
// (PlayerConnection); tests can implement fakes in-process.
//
// Deliberately free of any gRPC type so the matchmaker stays transport
// agnostic.

#include <functional>
#include <optional>
#include <string>

#include "cpp/tournament_server/tournament_broker.pb.h"

namespace tournament_broker {

class ClientHandle {
 public:
  virtual ~ClientHandle() = default;

  virtual auto name() const -> std::string = 0;

  // Queues a message for delivery. A true return means "accepted", not "on the
  // wire": writes complete asynchronously, and a delivery failure surfaces
  // later via disconnected(). Returns false when the connection is already
  // dead or closing.
  virtual auto Send(const proto::ServerMessage &msg) -> bool = 0;

  // Pops the next queued action, or nullopt when none is waiting. Never
  // blocks: the game learns that something arrived through the observer.
  virtual auto TryPopAction() -> std::optional<std::string> = 0;

  // Invoked from an arbitrary thread whenever something may have changed -- an
  // action arrived, or the connection dropped. Deliberately carries no
  // payload: the observer re-reads state instead, so a duplicated or
  // coalesced notification cannot lose an action. Pass nullptr to clear.
  virtual void SetObserver(std::function<void()> on_event) = 0;

  // Wakes the observer and fails future sends.
  virtual void MarkDisconnected() = 0;
  virtual auto disconnected() const -> bool = 0;

  // Flushes whatever is still queued and then ends the RPC. Idempotent.
  //
  // Replaces the old MarkDone()/WaitForDone() pair: the server now closes the
  // stream itself once the final GameOver has been written, instead of parking
  // a thread until the client happens to half-close.
  virtual void CloseAfterFlush() = 0;
};

}  // namespace tournament_broker

#endif  // RISK_GAME_AI_CPP_TOURNAMENT_SERVER_CLIENT_HANDLE_H
