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
// An order runs in a build container and then either a graded run or a match
// (a referee plus one or two bots). Timeouts are per phase; when one fires the
// docker client is killed and the container stopped by name.
//
// What the isolation actually rests on, now that the referee is here rather
// than on the coordinator:
//
//   - No network by default. The build and a graded run get --network=none;
//     a match gets a per-order --internal bridge, so the bots reach their
//     referee and nothing else. There is no longer an outside broker to dial,
//     which is what made --network=host necessary before.
//   - No capabilities. The overlay is assembled on the host, so the container
//     needs no CAP_SYS_ADMIN and runs --cap-drop=ALL with no-new-privileges.
//   - A read-only root filesystem, a non-root user, and cgroup caps on memory,
//     CPU and pids.
//
// The image is still trusted -- it carries bazel and the toolchain -- and a
// submitted genrule still runs arbitrary code at build time. The claim is that
// the code runs with nothing to reach and nothing to keep, not that it does not
// run.

#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "game_mcts/tournament_server/sandbox/worker/sandbox_backend.h"

namespace tournament_arena {

// Fixed mount points inside the containers.
inline constexpr char kDockerLowerMount[] = "/repo_lower";  // read-only clone
inline constexpr char kDockerPatchMount[] =
    "/patches";  // read-only order files
inline constexpr char kDockerWorkspace[] =
    "/workspace";  // overlay merge, repo root
inline constexpr char kDockerScratch[] = "/sandbox";  // upper/work, HOME
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
  // Host-side mount tooling for the per-slot overlay, configurable for the
  // same reason docker and git are: so a test can point them somewhere else.
  std::string mount = "mount";
  std::string umount = "umount";
  // Extra bazel flags, e.g. --config=native.
  std::vector<std::string> bazel_flags;
  // cgroup memory cap for the bot's container, in MiB. Zero disables it.
  int memory_limit_mb = 4096;

  // --- hardening, from ProblemConfig.sandbox ---

  // Assemble the overlay on the host and bind-mount the merged tree in. The
  // in-container alternative needs CAP_SYS_ADMIN, and a container with that
  // running submitted build code is not a boundary. Requires the worker to be
  // able to mount overlayfs -- root, or a user namespace.
  bool host_overlay = true;
  // Let the build container reach the network. A build that can fetch can also
  // exfiltrate, and a submitted genrule is arbitrary code.
  bool allow_build_network = false;
  // e.g. "1000:1000". Empty leaves the image's default, usually root.
  std::string run_as_user;
  double cpus = 0.0;     // 0: unlimited
  int pids_limit = 512;  // 0: unlimited
};

// Single-quote escaping for embedding an arbitrary string into the
// container's /bin/sh -c script.
auto ShellQuote(const std::string &value) -> std::string;

// Docker container names are [a-zA-Z0-9][a-zA-Z0-9_.-]*; anything else in an
// order id becomes '-'. The result needs no further escaping.
auto SanitizeContainerName(const std::string &value) -> std::string;

// The container-name prefix a slot uses for an order. Every container the
// backend starts for it begins with this, which is what lets Cancel find them
// without remembering anything.
auto ContainerNameBase(int slot, const std::string &order_id) -> std::string;

// Port the referee listens on inside the match network. Fixed rather than
// discovered: the network is private to one order, so nothing else is there to
// collide with.
inline constexpr int kMatchPort = 50051;

// The /bin/sh entrypoint of a build container: mount the slot's overlay, copy
// the staged candidate in, and run bazel build at the workspace root. Builds
// every target at once -- both sides of a match and the referee -- so they
// share one analysis pass and one consistent tree.
// |mount_in_container| assembles the overlay with mount(8) inside, which needs
// CAP_SYS_ADMIN. False means the host already mounted it and the merged tree is
// simply there.
auto DockerBuildScript(const std::filesystem::path &disk_cache,
                       const std::vector<std::string> &bazel_flags,
                       const std::vector<std::string> &patch_files,
                       const std::vector<std::string> &targets,
                       bool mount_in_container) -> std::string;

// The /bin/sh entrypoint of a run container: mount the same overlay and exec
// the built bot, whose bazel-bin symlink resolves into the shared output base.
auto DockerRunScript(const std::string &bot_path,
                     const std::vector<std::string> &args,
                     bool mount_in_container) -> std::string;

// The /bin/sh entrypoint of a graded run: the same overlay, $ARENA_REPORT
// pointing at where the command should write its numbers, then the command.
auto DockerGradeScript(const std::string &report_path,
                       const std::vector<std::string> &argv,
                       bool mount_in_container) -> std::string;

class DockerBackend final : public SandboxBackend {
 public:
  explicit DockerBackend(DockerBackendConfig config);

  auto RunOrder(int slot,
                const proto::WorkOrder &order) -> OrderOutcome override;
  auto name() const -> std::string override { return "docker"; }
  // Stops the order's containers. Needs no bookkeeping: every container this
  // backend starts is named from the slot and the order id, so the names are
  // derivable rather than remembered.
  void Cancel(const std::string &order_id) override;
  // Validates the repo and clones one lower dir per slot, so the first order
  // does not pay for the clone. Returns false with *error set.
  auto Warmup(int slots, std::string *error) -> bool override;

 private:
  auto SlotDir(int slot) const -> std::filesystem::path;
  auto RepoDir(int slot) const -> std::filesystem::path;
  auto OverlayDir(int slot) const -> std::filesystem::path;
  auto OutputBase(int slot) const -> std::filesystem::path;
  auto PatchDir(int slot) const -> std::filesystem::path;
  // Where the host-mounted overlay's merged tree lives, and what gets
  // bind-mounted in as the workspace.
  auto MergedDir(int slot) const -> std::filesystem::path;

  // Mounts (or unmounts) the slot's overlay on the host, so containers need no
  // CAP_SYS_ADMIN. Returns false with *error set -- there is deliberately no
  // fallback to the privileged form.
  auto MountOverlay(int slot, std::string *error) -> bool;
  void UnmountOverlay(int slot);

  // The `docker run` flags every container of an order shares: the isolation
  // this backend rests on, in one place so no call site can quietly omit one.
  auto HardeningArgs() const -> std::vector<std::string>;
  // The workspace mount, which differs by overlay mode.
  auto WorkspaceMounts(int slot) const -> std::vector<std::string>;

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
  // Runs a graded order's command |repeats| times in throwaway containers with
  // no network, and aggregates what it measured.
  auto RunGrade(int slot, const proto::WorkOrder &order, OrderOutcome outcome,
                const std::string &name_base,
                const std::filesystem::path &logs) -> OrderOutcome;

  const DockerBackendConfig config_;
  // Slots whose order is currently running, so Cancel knows which name to
  // derive. Small and short-lived; the names themselves are not stored.
  mutable std::mutex mutex_;
  std::map<std::string, int> running_slots_;
};

}  // namespace tournament_arena

#endif  // RISK_GAME_AI_CPP_TOURNAMENT_SERVER_SANDBOX_WORKER_DOCKER_BACKEND_H
