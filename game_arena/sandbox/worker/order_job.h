#ifndef GAME_ARENA_GAME_ARENA_SANDBOX_WORKER_ORDER_JOB_H
#define GAME_ARENA_GAME_ARENA_SANDBOX_WORKER_ORDER_JOB_H

// A work order as a sandbox job: where the arena's vocabulary meets exec/'s.

#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "game_arena/proto/arena.pb.h"
#include "game_arena/sandbox/exec/engine.h"
#include "game_arena/sandbox/exec/sandbox_job.pb.h"

namespace tournament_arena {

// Where the referee leaves its MatchReport, under its private {{scratch}}.
inline constexpr char kMatchReport[] = "match.pb";

// This host's layout only: what to build and how to sandbox it arrive on the
// order, since two submissions compare only if built the same way.
struct OrderJobConfig {
  std::filesystem::path work_dir;    // per-slot state under slot<N>
  std::filesystem::path disk_cache;  // the process engine's

  // A container's caches are volumes named from volume_prefix, or these dirs,
  // as the daemon resolves them, bind-mounted instead.
  std::string volume_prefix = "arena";
  std::filesystem::path bind_output_base_dir;  // gets a slot<N> subdirectory
  std::filesystem::path bind_disk_cache_dir;

  std::string git = "git";
  std::string bazel = "bazel";  // a container runs the image's
};

// A build order's stash, collected from its "stash" step.
inline constexpr char kStashedBot[] = "bot";
inline constexpr char kStashedReferee[] = "referee";

// A match's binaries from the archive, as files on this host: the job loads
// them into the sandbox and builds nothing.
struct Prebuilt {
  std::filesystem::path referee;
  std::map<std::string, std::filesystem::path> bots;  // by candidate id
};

// |capabilities| decides the rendezvous: a named referee on a fixed port, or a
// port file. A build_only order builds and stashes; with |prebuilt|, a match
// runs those binaries instead of building.
bool JobForOrder(int slot, const proto::WorkOrder &order,
                 const OrderJobConfig &config,
                 const sandbox_exec::Capabilities &capabilities,
                 sandbox_exec::proto::Job *job, std::string *error,
                 const Prebuilt *prebuilt = nullptr);

std::filesystem::path SlotLogDir(const OrderJobConfig &config, int slot);

}  // namespace tournament_arena

#endif  // GAME_ARENA_GAME_ARENA_SANDBOX_WORKER_ORDER_JOB_H
