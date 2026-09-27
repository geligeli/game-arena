#ifndef GAME_ARENA_GAME_ARENA_SANDBOX_COMMON_DOCKER_H
#define GAME_ARENA_GAME_ARENA_SANDBOX_COMMON_DOCKER_H

#include <filesystem>
#include <string>
#include <vector>

namespace sandbox_common {

// Mount points inside every container.
inline constexpr char kPatchMount[] = "/patches";   // read-only staged files
inline constexpr char kWorkspace[] = "/workspace";  // the tree, repo root
inline constexpr char kScratch[] = "/sandbox";      // writable; HOME
inline constexpr char kOutputBaseMount[] = "/output_base";
inline constexpr char kDiskCacheMount[] = "/disk_cache";

std::string ShellQuote(const std::string &value);

// Anything outside [a-zA-Z0-9_.-] becomes '-'.
std::string SanitizeContainerName(const std::string &value);

// --mount, not -v: -v silently creates a missing |source| as an empty dir.
std::string BindMount(const std::filesystem::path &source,
                      const std::string &target, bool readonly);

std::string VolumeMount(const std::string &volume, const std::string &target,
                        bool readonly);

// A writable HOME at kScratch: bazel needs one and the root is read-only.
std::string ScratchPrelude();

// Every `docker run`/`create` is spelled here, so none can omit a flag.
struct DockerRunSpec {
  std::string name;
  std::string image;
  std::string script;                   // run as /bin/sh -c <script>
  bool rm = true;                       // --rm, not with create
  bool detached = false;                // -d, not with create
  bool create = false;                  // `create` rather than `run`
  std::string network;                  // empty: docker's default
  std::vector<std::string> extra_args;  // ahead of the network and mounts
  std::vector<std::string> mounts;      // each already in --mount syntax
};

std::vector<std::string> DockerRunArgs(const DockerRunSpec &spec);

}  // namespace sandbox_common

#endif  // GAME_ARENA_GAME_ARENA_SANDBOX_COMMON_DOCKER_H
