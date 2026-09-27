#ifndef GAME_ARENA_GAME_ARENA_SANDBOX_EXEC_PROCESS_ENGINE_H
#define GAME_ARENA_GAME_ARENA_SANDBOX_EXEC_PROCESS_ENGINE_H

// Steps as plain subprocesses: limits and timeouts, not a security boundary.

#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "game_arena/sandbox/exec/engine.h"
#include "game_arena/sandbox/exec/sandbox_job.pb.h"

namespace sandbox_exec {

class ProcessEngine final : public Engine {
 public:
  Capabilities capabilities() const override {
    // The host's network: a fixed port would collide across parallel jobs.
    return Capabilities{/*isolates=*/false, /*stable_peer_names=*/false};
  }

  proto::JobResult Run(const proto::Job &job, Observer *observer) override;
  void Cancel(const std::string &job_id) override;

 private:
  bool RunPhase(const proto::Job &job, const proto::Phase &phase,
                Observer *observer, proto::PhaseResult *result,
                proto::Status *status);

  // Every running step's process group, background ones too, for Cancel.
  void Track(const std::string &job_id, pid_t pgid);
  void Untrack(const std::string &job_id, pid_t pgid);

  std::mutex mutex_;
  std::multimap<std::string, pid_t> running_;
  std::set<std::string> cancelled_;
};

}  // namespace sandbox_exec

#endif  // GAME_ARENA_GAME_ARENA_SANDBOX_EXEC_PROCESS_ENGINE_H
