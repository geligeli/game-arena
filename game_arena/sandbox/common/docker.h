#ifndef GAME_ARENA_GAME_ARENA_SANDBOX_COMMON_DOCKER_H
#define GAME_ARENA_GAME_ARENA_SANDBOX_COMMON_DOCKER_H

// Docker mechanics shared by the standalone sandbox runner (sandbox/runner)
// and the fleet worker's docker backend (sandbox/worker): how a container is
// named, how a bind mount is spelled, the fixed mount points inside the
// container, the shell prelude every entrypoint script starts with, and the
// one shape a `docker run` argv takes.
//
// One home so the two cannot drift into spelling the same container
// differently: the runner and a worker slot mount the same repository the
// same way, and a patch staged for one means the same thing to the other.

#include <chrono>
#include <filesystem>
#include <string>
#include <vector>

#include "game_arena/common/process/process.h"
#include "game_arena/sandbox/common/step.h"

namespace sandbox_common {

// Fixed mount points inside the containers.
inline constexpr char kPatchMount[] = "/patches";   // read-only staged files
inline constexpr char kWorkspace[] = "/workspace";  // the tree, repo root
inline constexpr char kScratch[] = "/sandbox";      // writable; HOME
inline constexpr char kOutputBaseMount[] = "/output_base";
inline constexpr char kDiskCacheMount[] = "/disk_cache";

// Single-quote escaping for embedding an arbitrary string into a container's
// /bin/sh -c script.
auto ShellQuote(const std::string &value) -> std::string;

// Docker container names are [a-zA-Z0-9][a-zA-Z0-9_.-]*; anything else becomes
// '-'. The result needs no further escaping.
auto SanitizeContainerName(const std::string &value) -> std::string;

// A bind mount in `docker run --mount` syntax. The long form is deliberate:
// `-v` creates |source| as an empty directory when it does not exist, which
// turns a mistyped host path into an empty repository or a run that silently
// drops every patch. --mount fails the run instead.
auto BindMount(const std::filesystem::path &source, const std::string &target,
               bool readonly) -> std::string;

// A named volume in the same syntax. Volumes belong to the daemon, so the
// engine never needs the daemon to see its own filesystem.
auto VolumeMount(const std::string &volume, const std::string &target,
                 bool readonly) -> std::string;

// The one line every entrypoint starts with after `set -eu`: a writable HOME,
// because bazel insists on one and the root filesystem is read-only. The
// tree is already at kWorkspace and scratch at kScratch; there is nothing to
// assemble.
auto ScratchPrelude() -> std::string;

// `docker volume create <name>` and `docker volume rm -f <name>`. Removing one
// that is already gone is a no-op.
auto CreateVolume(const std::string &docker, const std::string &name,
                  const std::filesystem::path &log_dir,
                  const std::string &tag) -> StepResult;
auto RemoveVolume(const std::string &docker,
                  const std::string &name) -> process::RunResult;

// Stops a container by name, with a bounded wait on the daemon. Killing one
// that already exited is a no-op, which is exactly the race a cancel or a
// timeout cleanup wants.
auto KillContainer(const std::string &docker,
                   const std::string &name) -> process::RunResult;

// Force-removes a container by name. Used to clear what a worker killed
// mid-order left behind, so a redelivered order starts fresh.
auto RemoveContainer(const std::string &docker,
                     const std::string &name) -> process::RunResult;

// `docker wait <name>`: block on the daemon until the container exits, rather
// than polling it. |timeout| bounds the wait itself, so a container that never
// exits cannot hold the caller.
auto WaitForContainer(const std::string &docker, const std::string &name,
                      std::chrono::seconds timeout,
                      const std::filesystem::path &log_dir,
                      const std::string &tag) -> StepResult;

// `docker logs <name>`: what a container printed, read back after it exited.
// This is how a detached container's verdict gets home -- it is started with
// nobody attached to its stdout, so the output has to be asked for.
auto ContainerLogs(const std::string &docker, const std::string &name,
                   const std::filesystem::path &log_dir,
                   const std::string &tag) -> StepResult;

// `docker network create --internal <name>`: a bridge with no egress. The
// containers on it reach each other and nothing else, which is what a match
// needs and the most a match may have.
auto CreateInternalNetwork(const std::string &docker, const std::string &name,
                           const std::filesystem::path &log_dir,
                           const std::string &tag) -> StepResult;

// `docker network rm <name>`. Removing one that is already gone is a no-op,
// the same race KillContainer is written for.
auto RemoveNetwork(const std::string &docker,
                   const std::string &name) -> process::RunResult;

// One `docker run` (or `docker create`) invocation. The flag order is fixed
// here -- call sites express only what differs between a build, a graded run
// and a match container, so no call site can quietly omit a flag.
struct DockerRunSpec {
  std::string name;       // --name
  std::string image;      // the image every container starts from
  std::string script;     // run as /bin/sh -c <script>
  bool rm = true;         // --rm: throwaway container
  bool detached = false;  // -d: returns immediately rather than blocking
  bool create = false;    // `create` rather than `run`: made, not started
  std::string network;    // empty: docker's default; else --network <network>
  std::vector<std::string> extra_args;  // hardening etc., ahead of the mounts
  std::vector<std::string> mounts;      // each already in --mount syntax
};

// The full argv for `docker run`: run [--rm] --name N [-d] <extra_args>
// [--network n] --mount... --entrypoint /bin/sh <image> -c <script>.
auto DockerRunArgs(const DockerRunSpec &spec) -> std::vector<std::string>;

}  // namespace sandbox_common

#endif  // GAME_ARENA_GAME_ARENA_SANDBOX_COMMON_DOCKER_H
