#ifndef RISK_GAME_AI_CPP_TOURNAMENT_SERVER_SANDBOX_WORKER_LOCAL_BACKEND_H
#define RISK_GAME_AI_CPP_TOURNAMENT_SERVER_SANDBOX_WORKER_LOCAL_BACKEND_H

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
// Candidate code is compiled and run as the worker's own user; treat
// submissions as trusted until the docker backend is in use.

#include <filesystem>
#include <string>

#include "game_mcts/tournament_server/sandbox_worker/sandbox_backend.h"

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
  auto name() const -> std::string override { return "local"; }

  // Prepares |slots| checkouts up front, so the first order does not pay for
  // the clone. Returns false with *error set when the repo cannot be cloned.
  auto Warmup(int slots, std::string *error) -> bool;

 private:
  auto SlotDir(int slot) const -> std::filesystem::path;
  auto RepoDir(int slot) const -> std::filesystem::path;
  auto OutputBase(int slot) const -> std::filesystem::path;

  // Clones on first use, then resets to |base_commit| and removes any previous
  // candidate. Returns false with *error set.
  auto PrepareCheckout(int slot, const std::string &base_commit,
                       std::string *error) -> bool;
  auto WriteCandidate(int slot, const proto::WorkOrder &order,
                      std::string *error) -> bool;

  const LocalBackendConfig config_;
};

}  // namespace tournament_arena

#endif  // RISK_GAME_AI_CPP_TOURNAMENT_SERVER_SANDBOX_WORKER_LOCAL_BACKEND_H
