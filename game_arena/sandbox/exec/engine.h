#ifndef GAME_ARENA_GAME_ARENA_SANDBOX_EXEC_ENGINE_H
#define GAME_ARENA_GAME_ARENA_SANDBOX_EXEC_ENGINE_H

// Runs a job in a sandbox. No order, game or bazel semantics belong in exec/.

#include <filesystem>
#include <map>
#include <string>

#include "game_arena/sandbox/exec/sandbox_job.pb.h"

namespace sandbox_exec {

struct Capabilities {
  bool isolates = false;  // a security boundary; submitted code requires it
  bool stable_peer_names = false;  // a peer is reachable by name, no discovery
};

class Observer {
 public:
  virtual ~Observer() = default;
  virtual void OnWorkspaceReady(const std::string &job_id) { (void)job_id; }
  virtual void OnPhaseStarted(const std::string &job_id,
                              const std::string &phase) {
    (void)job_id;
    (void)phase;
  }
};

// Deterministic, so a redelivered job can reap what a killed attempt left.
std::string SandboxName(const std::string &job_id,
                        const std::string &step_name);

// Resolved by the engine: {{scratch}} (where collect_files are read from),
// {{port_file}} and {{peer:<name>}} (process engine only). Unknown ones stay.
inline constexpr char kScratchPlaceholder[] = "{{scratch}}";
inline constexpr char kPortFilePlaceholder[] = "{{port_file}}";

// In argv and env both: a report path travels in env, a peer in argv.
proto::Step Substituted(const proto::Step &step,
                        const std::map<std::string, std::string> &replacements);

// Sets |status|; always false.
bool Fail(proto::Status *status, proto::Status::Code code,
          const std::string &message);

// Wholesale, never per field: half-overridden isolation looks tight and isn't.
proto::Isolation Merge(const proto::Isolation &base,
                       const proto::Isolation &override_with);

// A started step's result, its output read from <log_dir>/<name>.{out,err}.
proto::StepResult *AddStepResult(proto::PhaseResult *phase,
                                 const std::string &name,
                                 const std::filesystem::path &log_dir);

class Engine {
 public:
  virtual ~Engine() = default;

  virtual Capabilities capabilities() const = 0;

  // Safe to call concurrently for jobs with different ids.
  virtual proto::JobResult Run(const proto::Job &job, Observer *observer) = 0;

  // From another thread, racing Run's exit: killing what exited is a no-op.
  virtual void Cancel(const std::string &job_id) = 0;
};

}  // namespace sandbox_exec

#endif  // GAME_ARENA_GAME_ARENA_SANDBOX_EXEC_ENGINE_H
