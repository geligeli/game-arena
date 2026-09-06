#ifndef RISK_GAME_AI_CPP_TOURNAMENT_SERVER_SANDBOX_WORKER_DOCKER_BACKEND_H
#define RISK_GAME_AI_CPP_TOURNAMENT_SERVER_SANDBOX_WORKER_DOCKER_BACKEND_H

// Runs orders in throwaway docker containers.
//
// Each slot owns three persistent host directories, reused across orders:
//
//   repo/                a clone of --repo, the overlay's lower dir. The host
//                        does the git work (clone, fetch, checkout), so the
//                        lower dir is always a clean tree at the order's
//                        base_commit and the containers never touch .git.
//   overlay/             the overlay's upper/work dirs, bind-mounted into the
//                        container. Candidate files, the generated BUILD and
//                        bazel's workspace symlinks land here, so a slot's
//                        built state survives between orders.
//   bazel_output_base/   a persistent per-slot bazel output base, so a warm
//                        slot compiles only the submitted files.
//
// Every order runs in two containers over the same overlay: one builds the
// candidate target, one runs the built bot against the broker. Timeouts are
// per phase (build_timeout_s / run_timeout_s); when one fires the docker
// client is killed and the container stopped by name, as in sandbox_runner.
//
// Isolation is stronger than the local backend's -- separate filesystem view,
// a cgroup memory cap on the bot, containers killed on timeout -- but it is
// still not a security boundary: containers run with host networking so the
// bot can reach the broker, and the image must be trusted (it carries bazel
// and the toolchain). Submissions are untrusted input to a trusted image.

#include <filesystem>
#include <string>
#include <vector>

#include "game_mcts/tournament_server/sandbox/worker/sandbox_backend.h"

namespace tournament_arena {

// Fixed mount points inside the containers.
inline constexpr char kDockerLowerMount[] = "/repo_lower";  // read-only clone
inline constexpr char kDockerPatchMount[] = "/patches";     // read-only order files
inline constexpr char kDockerWorkspace[] = "/workspace";    // overlay merge, repo root
inline constexpr char kDockerScratch[] = "/sandbox";        // upper/work, HOME
inline constexpr char kDockerOutputBaseMount[] = "/output_base";
inline constexpr char kDockerDiskCacheMount[] = "/disk_cache";

struct DockerBackendConfig {
  // A local git checkout, mounted read-only into every container. Must be a
  // directory on this host: it is a bind-mount source.
  std::filesystem::path repo_dir;
  std::filesystem::path work_dir;
  // Shared by every slot, so a cold slot still gets warm artifacts.
  std::filesystem::path disk_cache;
  std::string docker = "docker";
  // The image every container starts from. Must contain bazel (matching the
  // repo's MODULE.bazel.lock), a C++ toolchain, and /bin/sh + coreutils +
  // mount(8) for the in-container overlay.
  std::string docker_image;
  // Host-side git for the per-slot clones.
  std::string git = "git";
  // Extra bazel flags, e.g. --config=native.
  std::vector<std::string> bazel_flags;
  // cgroup memory cap for the bot's container, in MiB. Zero disables it.
  int memory_limit_mb = 4096;
};

// Single-quote escaping for embedding an arbitrary string into the
// container's /bin/sh -c script.
auto ShellQuote(const std::string &value) -> std::string;

// Docker container names are [a-zA-Z0-9][a-zA-Z0-9_.-]*; anything else in an
// order id becomes '-'. The result needs no further escaping.
auto SanitizeContainerName(const std::string &value) -> std::string;

// The /bin/sh entrypoint of a build container: mount the slot's overlay, copy
// the staged candidate in, and run bazel build at the workspace root.
auto DockerBuildScript(const std::filesystem::path &disk_cache,
                       const std::vector<std::string> &bazel_flags,
                       const std::string &target) -> std::string;

// The /bin/sh entrypoint of a run container: mount the same overlay and exec
// the built bot, whose bazel-bin symlink resolves into the shared output base.
auto DockerRunScript(const std::string &bot_path,
                     const std::vector<std::string> &args) -> std::string;

class DockerBackend final : public SandboxBackend {
 public:
  explicit DockerBackend(DockerBackendConfig config);

  auto RunOrder(int slot,
                const proto::WorkOrder &order) -> OrderOutcome override;
  auto name() const -> std::string override { return "docker"; }
  // Validates the repo and clones one lower dir per slot, so the first order
  // does not pay for the clone. Returns false with *error set.
  auto Warmup(int slots, std::string *error) -> bool override;

 private:
  auto SlotDir(int slot) const -> std::filesystem::path;
  auto RepoDir(int slot) const -> std::filesystem::path;
  auto OverlayDir(int slot) const -> std::filesystem::path;
  auto OutputBase(int slot) const -> std::filesystem::path;
  auto PatchDir(int slot) const -> std::filesystem::path;

  // Clones the repo into the slot's lower dir on first use.
  auto CloneSlot(int slot, std::string *error) -> bool;
  // Clone plus fetch/checkout of |base_commit|, mirroring LocalBackend.
  auto PrepareCheckout(int slot, const std::string &base_commit,
                       std::string *error) -> bool;
  // Writes the order's files and generated BUILD under the slot's patch dir
  // (mounted read-only into the container), and clears the slot's overlay
  // upper of the previous order's candidate.
  auto StageCandidate(int slot, const proto::WorkOrder &order,
                      std::string *error) -> bool;

  const DockerBackendConfig config_;
};

}  // namespace tournament_arena

#endif  // RISK_GAME_AI_CPP_TOURNAMENT_SERVER_SANDBOX_WORKER_DOCKER_BACKEND_H
