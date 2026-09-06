#ifndef GAME_MCTS_GAME_MCTS_TOURNAMENT_SERVER_SANDBOX_COMMON_DOCKER_H
#define GAME_MCTS_GAME_MCTS_TOURNAMENT_SERVER_SANDBOX_COMMON_DOCKER_H

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

#include "game_mcts/common/process/process.h"

namespace sandbox_common {

// Fixed mount points inside the containers.
inline constexpr char kLowerMount[] = "/repo_lower";  // read-only repo
inline constexpr char kPatchMount[] = "/patches";     // read-only order files
inline constexpr char kWorkspace[] = "/workspace";  // overlay merge, repo root
inline constexpr char kScratch[] = "/sandbox";      // overlay upper/work, HOME
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

// The scratch dirs and a writable HOME, without any mount: a container
// building against its own filesystem still needs both (bazel insists on a
// writable HOME). Shared prelude of OverlayMountScript.
auto ScratchSetupScript() -> std::string;

// ScratchSetupScript plus the overlay mount itself. The upper and work dirs
// sit under the one scratch mount so they are guaranteed to share a
// filesystem, which overlayfs requires. Needs CAP_SYS_ADMIN; the host-mounted
// form below is the hardened default.
auto OverlayMountScript() -> std::string;

// With the overlay mounted on the host the merged tree is simply at
// kWorkspace and there is nothing to assemble -- but the container still
// needs a writable HOME, because its root filesystem is read-only.
auto HostOverlayPrelude() -> std::string;

// Stops a container by name, with a bounded wait on the daemon. Killing one
// that already exited is a no-op, which is exactly the race a cancel or a
// timeout cleanup wants.
auto KillContainer(const std::string &docker,
                   const std::string &name) -> process::RunResult;

// Force-removes a container by name. Used to clear what a worker killed
// mid-order left behind, so a redelivered order starts fresh.
auto RemoveContainer(const std::string &docker,
                     const std::string &name) -> process::RunResult;

// One `docker run` invocation. The flag order is fixed here -- call sites
// express only what differs between a build, a graded run and a match
// container, so no call site can quietly omit a flag.
struct DockerRunSpec {
  std::string name;       // --name
  std::string image;      // the image every container starts from
  std::string script;     // run as /bin/sh -c <script>
  bool rm = true;         // --rm: throwaway container
  bool detached = false;  // -d: returns immediately rather than blocking
  std::string network;    // empty: docker's default; else --network <network>
  std::vector<std::string> extra_args;  // hardening etc., ahead of the mounts
  std::vector<std::string> mounts;      // each already in --mount syntax
};

// The full argv for `docker run`: run [--rm] --name N [-d] <extra_args>
// [--network n] --mount... --entrypoint /bin/sh <image> -c <script>.
auto DockerRunArgs(const DockerRunSpec &spec) -> std::vector<std::string>;

}  // namespace sandbox_common

#endif  // GAME_MCTS_GAME_MCTS_TOURNAMENT_SERVER_SANDBOX_COMMON_DOCKER_H
