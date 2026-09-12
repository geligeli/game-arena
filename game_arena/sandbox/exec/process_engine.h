#ifndef GAME_ARENA_GAME_ARENA_SANDBOX_EXEC_PROCESS_ENGINE_H
#define GAME_ARENA_GAME_ARENA_SANDBOX_EXEC_PROCESS_ENGINE_H

// Runs a job's steps as plain subprocesses.
//
// Resource limits and timeouts, **not a security boundary**: the steps run as
// this process's own user, on this process's own filesystem. Enough to stop a
// runaway step, and nothing at all against one that means harm --
// capabilities().isolates is false, and a caller whose job must be contained
// is expected to check it rather than to know which engine it was handed.
//
// The reason it exists is the development loop: no docker daemon, no image to
// keep warm, no overlay to mount, and a stack trace where you can see it.

#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "game_arena/sandbox/exec/engine.h"
#include "game_arena/sandbox/exec/sandbox_job.pb.h"

namespace sandbox_exec {

struct ProcessEngineConfig {
  // How long to wait for a background step to publish its port before giving
  // up on the phase.
  int endpoint_timeout_s = 60;
};

class ProcessEngine final : public Engine {
 public:
  explicit ProcessEngine(ProcessEngineConfig config = {});

  auto name() const -> std::string override { return "process"; }
  auto capabilities() const -> Capabilities override {
    // Steps share the host's network, so they can reach each other -- but
    // only by a port discovered at runtime, because parallel jobs on one host
    // would collide on a fixed one.
    return Capabilities{/*isolates=*/false, /*shared_network=*/true,
                        /*stable_peer_names=*/false};
  }

  auto Run(const proto::Job &job,
           Observer *observer) -> proto::JobResult override;
  void Cancel(const std::string &job_id) override;

 private:
  auto RunPhase(const proto::Job &job, const proto::Phase &phase,
                Observer *observer, proto::PhaseResult *result,
                proto::Status *status) -> bool;

  // Publishes a step's process group against |job_id| while it runs, so a
  // Cancel from another thread can reach the whole tree. Every step, not just
  // the last one: a phase's background steps are processes too, and before
  // this engine a cancelled local match left its referee running.
  void Track(const std::string &job_id, pid_t pgid);
  void Untrack(const std::string &job_id, pid_t pgid);

  const ProcessEngineConfig config_;
  std::mutex mutex_;
  std::multimap<std::string, pid_t> running_;
  std::set<std::string> cancelled_;
};

}  // namespace sandbox_exec

#endif  // GAME_ARENA_GAME_ARENA_SANDBOX_EXEC_PROCESS_ENGINE_H
