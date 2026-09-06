#ifndef GAME_MCTS_GAME_MCTS_TOURNAMENT_SERVER_SANDBOX_WORKER_LOCAL_BACKEND_H
#define GAME_MCTS_GAME_MCTS_TOURNAMENT_SERVER_SANDBOX_WORKER_LOCAL_BACKEND_H

// Runs orders as local subprocesses.
//
// Each slot owns a persistent checkout and a persistent bazel output base,
// reused across orders. That is the difference between a candidate build
// taking seconds and taking minutes: a fresh output base re-analyses the whole
// workspace and re-links every dependency, while a warm one only compiles the
// submitted files. Slots never share either, so concurrent builds do not
// serialise on bazel's workspace lock.
//
// Isolation here is resource limits and timeouts, not a security boundary.
// Candidate code is compiled and run as the worker's own user. The docker
// backend (docker_backend.h) runs the same steps in hardened containers -- no
// network, no capabilities, cgroup caps -- and a problem can refuse this
// backend entirely with SandboxSpec.require_container.

#include <sys/types.h>

#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <string>

#include "game_mcts/tournament_server/sandbox/worker/sandbox_backend.h"

namespace tournament_arena {

struct LocalBackendConfig {
  // Cloned once per slot. A local path is cloned with --local so objects are
  // hardlinked rather than copied.
  std::string repo_url;
  std::filesystem::path work_dir;
  // Shared by every slot, so a cold slot still gets warm artifacts.
  std::filesystem::path disk_cache;
  std::string bazel = "bazel";
  std::string git = "git";
  // Extra bazel flags, e.g. --config=native.
  std::vector<std::string> bazel_flags;
  // Address-space cap for the bot process, in MiB. Zero disables it.
  int memory_limit_mb = 4096;
};

class LocalBackend final : public SandboxBackend {
 public:
  explicit LocalBackend(LocalBackendConfig config);

  auto RunOrder(int slot,
                const proto::WorkOrder &order) -> OrderOutcome override;
  // Kills the order's current step by process group, which reaches the whole
  // tree -- bazel spawns one, and killing only the parent leaves the workers
  // building.
  void Cancel(const std::string &order_id) override;

  // Publishes a step's process group against |order_id| while it runs, so a
  // Cancel from the stream thread can reach it. Public only so the scope guard
  // in the .cc can reach Untrack.
  auto Track(const std::string &order_id) -> std::function<void(pid_t)>;
  void Untrack(const std::string &order_id);
  auto name() const -> std::string override { return "local"; }

  // Prepares |slots| checkouts up front, so the first order does not pay for
  // the clone. Returns false with *error set when the repo cannot be cloned.
  auto Warmup(int slots, std::string *error) -> bool override;

 private:
  auto SlotDir(int slot) const -> std::filesystem::path;
  auto RepoDir(int slot) const -> std::filesystem::path;
  auto OutputBase(int slot) const -> std::filesystem::path;

  // Clones on first use, then resets to |base_commit| and removes any previous
  // candidate. Returns false with *error set.
  auto PrepareCheckout(int slot, const std::string &base_commit,
                       std::string *error) -> bool;
  // Applies one side's patch with git. Both sides of a match land in the same
  // checkout, so the second one can conflict; that is reported as itself.
  auto ApplySide(int slot, const proto::Side &side, std::string *error) -> bool;
  // Runs a graded order's command |repeats| times and aggregates what it
  // measured. |outcome| arrives with build_ok already decided.
  auto RunGrade(int slot, const proto::WorkOrder &order,
                OrderOutcome outcome) -> OrderOutcome;

  const LocalBackendConfig config_;

  // order id -> the process group of the step it is running, so a cancel can
  // reach work already under way. An order with no entry is either queued or
  // between steps, and Cancel then has nothing to do.
  mutable std::mutex mutex_;
  std::map<std::string, pid_t> running_;
};

}  // namespace tournament_arena

#endif  // GAME_MCTS_GAME_MCTS_TOURNAMENT_SERVER_SANDBOX_WORKER_LOCAL_BACKEND_H
