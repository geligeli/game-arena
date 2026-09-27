#ifndef GAME_ARENA_GAME_ARENA_SANDBOX_COMMON_DOCKER_H
#define GAME_ARENA_GAME_ARENA_SANDBOX_COMMON_DOCKER_H

// Docker mechanics: container names, mount spellings, fixed mount points,
// the entrypoint prelude and the one shape a `docker run` argv takes.

#include <filesystem>
#include <string>
#include <vector>

namespace sandbox_common {

// Fixed mount points inside the containers.
inline constexpr char kPatchMount[] = "/patches";   // read-only staged files
inline constexpr char kWorkspace[] = "/workspace";  // the tree, repo root
inline constexpr char kScratch[] = "/sandbox";      // writable; HOME
inline constexpr char kOutputBaseMount[] = "/output_base";
inline constexpr char kDiskCacheMount[] = "/disk_cache";

// Single-quote escaping for embedding an arbitrary string into a container's
// /bin/sh -c script.
std::string ShellQuote(const std::string &value);

// Docker container names are [a-zA-Z0-9][a-zA-Z0-9_.-]*; anything else becomes
// '-'. The result needs no further escaping.
std::string SanitizeContainerName(const std::string &value);

// A bind mount in `docker run --mount` syntax. The long form is deliberate:
// `-v` creates |source| as an empty directory when it does not exist, which
// turns a mistyped host path into an empty repository or a run that silently
// drops every patch. --mount fails the run instead.
std::string BindMount(const std::filesystem::path &source,
                      const std::string &target, bool readonly);

// A named volume in the same syntax. Volumes belong to the daemon, so the
// engine never needs the daemon to see its own filesystem.
std::string VolumeMount(const std::string &volume, const std::string &target,
                        bool readonly);

// The one line every entrypoint starts with after `set -eu`: a writable HOME,
// because bazel insists on one and the root filesystem is read-only. The
// tree is already at kWorkspace and scratch at kScratch; there is nothing to
// assemble.
std::string ScratchPrelude();

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
std::vector<std::string> DockerRunArgs(const DockerRunSpec &spec);

}  // namespace sandbox_common

#endif  // GAME_ARENA_GAME_ARENA_SANDBOX_COMMON_DOCKER_H
