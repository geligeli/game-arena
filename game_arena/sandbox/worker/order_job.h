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
  // Per-slot state lives under <work_dir>/slot<N>: a persistent checkout,
  // and for the process engine a persistent bazel output base, reused across
  // orders. The difference between a candidate build taking seconds and
  // taking minutes.
  std::filesystem::path work_dir;
  // The process engine's shared bazel disk cache.
  std::filesystem::path disk_cache;

  // A container's persistent state lives in docker volumes by default, named
  // <volume_prefix>-slot<N>-output_base and <volume_prefix>-disk_cache: the
  // daemon keeps them, and nothing about this process's filesystem has to be
  // visible to it. A host that wants those caches on a local disk it can
  // inspect (or share with its own builds) names the directories here, as
  // the *daemon* resolves them, and they are bind-mounted instead. Purely a
  // performance choice; the default works everywhere a docker socket does.
  std::string volume_prefix = "arena";
  std::filesystem::path bind_output_base_dir;  // gets a slot<N> subdirectory
  std::filesystem::path bind_disk_cache_dir;

  std::string git = "git";
  // The build tool. A path for the process engine; inside a container it is
  // whatever the image calls bazel.
  std::string bazel = "bazel";
};

// Builds the job for |order| in |slot|. |capabilities| decides the rendezvous:
// an engine with stable peer names gets a referee on a fixed port addressed by
// name, and one without gets the port-file dance.
bool JobForOrder(int slot, const proto::WorkOrder &order,
                 const OrderJobConfig &config,
                 const sandbox_exec::Capabilities &capabilities,
                 sandbox_exec::proto::Job *job, std::string *error);

// Where |slot|'s logs go, which is also where the engine writes them.
std::filesystem::path SlotLogDir(const OrderJobConfig &config, int slot);

}  // namespace tournament_arena

#endif  // GAME_ARENA_GAME_ARENA_SANDBOX_WORKER_ORDER_JOB_H
