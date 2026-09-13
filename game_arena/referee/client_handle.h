#ifndef GAME_ARENA_GAME_ARENA_REFEREE_CLIENT_HANDLE_H
#define GAME_ARENA_GAME_ARENA_REFEREE_CLIENT_HANDLE_H

// Abstraction over one connected player, independent of the transport. The
// gRPC front end implements it over a bidirectional stream
// (PlayerConnection); tests can implement fakes in-process.
//
// Deliberately free of any gRPC type so the matchmaker stays transport
// agnostic.

#include <functional>
#include <optional>
#include <string>

#include "game_arena/proto/tournament_broker.pb.h"

namespace tournament_broker {

class ClientHandle {
 public:
  virtual ~ClientHandle() = default;

  virtual std::string name() const = 0;

  // Queues a message for delivery. A true return means "accepted", not "on the
  // wire": writes complete asynchronously, and a delivery failure surfaces
  // later via disconnected(). Returns false when the connection is already
  // dead or closing.
  virtual bool Send(const proto::ServerMessage &msg) = 0;

  // Pops the next queued action, or nullopt when none is waiting. Never
  // blocks: the game learns that something arrived through the observer.
  virtual std::optional<std::string> TryPopAction() = 0;

  // Invoked from an arbitrary thread whenever something may have changed -- an
  // action arrived, or the connection dropped. Deliberately carries no
  // payload: the observer re-reads state instead, so a duplicated or
  // coalesced notification cannot lose an action. Pass nullptr to clear.
  virtual void SetObserver(std::function<void()> on_event) = 0;

  // Wakes the observer and fails future sends.
  virtual void MarkDisconnected() = 0;
  virtual bool disconnected() const = 0;

  // Flushes whatever is still queued and then ends the RPC. Idempotent.
  //
  // Replaces the old MarkDone()/WaitForDone() pair: the server now closes the
  // stream itself once the final GameOver has been written, instead of parking
  // a thread until the client happens to half-close.
  virtual void CloseAfterFlush() = 0;
};

}  // namespace tournament_broker

#endif  // GAME_ARENA_GAME_ARENA_REFEREE_CLIENT_HANDLE_H
