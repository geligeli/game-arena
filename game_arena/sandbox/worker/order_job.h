#ifndef GAME_ARENA_GAME_ARENA_SANDBOX_WORKER_ORDER_JOB_H
#define GAME_ARENA_GAME_ARENA_SANDBOX_WORKER_ORDER_JOB_H

// Turning a work order into a sandbox job.
//
// The one place the arena's vocabulary meets the engine's, and therefore the
// one place that knows a match has a referee, that a bot is dialled with
// --server, that a graded run writes $ARENA_REPORT, and that a bazel label
// becomes a path under bazel-bin. Everything below this is generic
// (//game_arena/sandbox/exec); everything above it is the fleet protocol.
//
// Written once for both engines, where the two backends each had their own
// copy. The difference between them is genuinely small and genuinely real:
// a container gets its own network namespace, so a referee can listen on a
// fixed port and be reached by name, while parallel slots on one host must
// bind port 0 and publish the number in a file.

#include <filesystem>
#include <string>
#include <vector>

#include "game_arena/proto/arena.pb.h"
#include "game_arena/sandbox/exec/engine.h"
#include "game_arena/sandbox/exec/sandbox_job.pb.h"

namespace tournament_arena {

// What only this host knows.
//
// Everything about *what* to build and *what the sandbox may do* arrives on
// the order, because two submissions are only comparable if they were built
// the same way. What is left here is this machine's own layout: where to put
// the slots, and what its tools are called.
//
// The tool paths are not worker flags either. They exist as fields because
// the whole sandbox test suite injects fake `docker`, `git` and `mount`
// scripts through them and asserts the exact argv without a daemon.
struct OrderJobConfig {
  // Per-slot state lives under <work_dir>/slot<N>: a persistent checkout and
  // a persistent bazel output base, reused across orders. The difference
  // between a candidate build taking seconds and taking minutes.
  std::filesystem::path work_dir;
  std::filesystem::path disk_cache;
  std::string git = "git";
  std::string mount = "mount";
  std::string umount = "umount";
  // The build tool. A path for the process engine; inside a container it is
  // whatever the image calls bazel.
  std::string bazel = "bazel";
};

// Builds the job for |order| in |slot|. |capabilities| decides the rendezvous:
// an engine with stable peer names gets a referee on a fixed port addressed by
// name, and one without gets the port-file dance.
auto JobForOrder(int slot, const proto::WorkOrder &order,
                 const OrderJobConfig &config,
                 const sandbox_exec::Capabilities &capabilities,
                 sandbox_exec::proto::Job *job, std::string *error) -> bool;

// Where |slot|'s logs go, which is also where the engine writes them.
auto SlotLogDir(const OrderJobConfig &config,
                int slot) -> std::filesystem::path;

}  // namespace tournament_arena

#endif  // GAME_ARENA_GAME_ARENA_SANDBOX_WORKER_ORDER_JOB_H
