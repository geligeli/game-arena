#ifndef GAME_ARENA_GAME_ARENA_SERVER_FLEET_WORKER_H
#define GAME_ARENA_GAME_ARENA_SERVER_FLEET_WORKER_H

// An attached sandbox worker, gRPC-free so tests can drive a fake.

#include <string>

#include "game_arena/proto/arena.pb.h"

namespace tournament_arena {

class FleetWorker {
 public:
  virtual ~FleetWorker() = default;

  // Stable across reconnects, so a returning worker is not counted twice.
  virtual std::string worker_id() const = 0;
  virtual int slots() const = 0;
  // False means the worker is gone and its orders should be requeued.
  virtual bool Send(const proto::FleetMessage &msg) = 0;
};

}  // namespace tournament_arena

#endif  // GAME_ARENA_GAME_ARENA_SERVER_FLEET_WORKER_H
