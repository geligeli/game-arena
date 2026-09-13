#ifndef GAME_ARENA_GAME_ARENA_SERVER_FLEET_WORKER_H
#define GAME_ARENA_GAME_ARENA_SERVER_FLEET_WORKER_H

// One attached sandbox worker, as the scheduler sees it.
//
// Same split as ClientHandle: the scheduler never touches a gRPC type, so it
// can be driven by a fake in tests and, in principle, by a transport other
// than the fleet stream. A worker is reached only through Send().

#include <string>

#include "game_arena/proto/arena.pb.h"

namespace tournament_arena {

class FleetWorker {
 public:
  virtual ~FleetWorker() = default;

  // Stable across reconnects, so a returning worker is recognised rather than
  // counted twice.
  virtual std::string worker_id() const = 0;

  // How many orders this worker will run at once.
  virtual int slots() const = 0;

  // Queues a message. False means the worker is gone and its in-flight orders
  // should be requeued.
  virtual bool Send(const proto::FleetMessage &msg) = 0;
};

}  // namespace tournament_arena

#endif  // GAME_ARENA_GAME_ARENA_SERVER_FLEET_WORKER_H
