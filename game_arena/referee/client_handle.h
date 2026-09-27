#ifndef GAME_ARENA_GAME_ARENA_REFEREE_CLIENT_HANDLE_H
#define GAME_ARENA_GAME_ARENA_REFEREE_CLIENT_HANDLE_H

// One player, free of gRPC types so the matchmaker stays transport agnostic.

#include <functional>
#include <optional>
#include <string>

#include "game_arena/proto/tournament_broker.pb.h"

namespace tournament_broker {

class ClientHandle {
 public:
  virtual ~ClientHandle() = default;

  virtual std::string name() const = 0;

  // True means queued, not sent: a failed write shows up as disconnected().
  virtual bool Send(const proto::ServerMessage &msg) = 0;

  virtual std::optional<std::string> TryPopAction() = 0;

  // Runs on any thread when something may have changed. No payload, so a
  // coalesced wakeup loses nothing. nullptr clears it.
  virtual void SetObserver(std::function<void()> on_event) = 0;

  // Wakes the observer and fails future sends.
  virtual void MarkDisconnected() = 0;
  virtual bool disconnected() const = 0;

  // Flushes what is queued, then ends the RPC. Idempotent.
  virtual void CloseAfterFlush() = 0;
};

}  // namespace tournament_broker

#endif  // GAME_ARENA_GAME_ARENA_REFEREE_CLIENT_HANDLE_H
