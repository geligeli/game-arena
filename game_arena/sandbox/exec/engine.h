#ifndef GAME_ARENA_GAME_ARENA_SANDBOX_EXEC_ENGINE_H
#define GAME_ARENA_GAME_ARENA_SANDBOX_EXEC_ENGINE_H

// Running a job in a sandbox, with no idea what the job is for.
//
// One interface, two implementations: a container engine that is a security
// boundary, and a process engine that is resource limits and timeouts. A
// caller states what to run (sandbox_job.proto) and reads back what happened;
// it never spells a docker flag, mounts an overlay, or decides how to kill a
// process tree.
//
// What this deliberately does not know: orders, candidates, submissions,
// games, referees, ELO, metrics, or bazel. A step is argv plus an
// environment; that the argv happens to start with "bazel" is the caller's
// business.

#include <filesystem>
#include <functional>
#include <map>
#include <string>

#include "game_arena/sandbox/exec/sandbox_job.pb.h"

namespace sandbox_exec {

// What an engine can promise, so a caller can refuse a job this engine cannot
// contain without having to ask what the engine is called.
struct Capabilities {
  // A security boundary, not merely resource limits. A caller that runs
  // submitted code which may execute at build time must require this.
  bool isolates = false;
  // A peer is addressable before it starts, so argv can name it up front
  // rather than discovering an address at runtime.
  bool stable_peer_names = false;
};

// Progress, in the engine's own vocabulary. A caller maps these onto whatever
// phases mean to it; the engine does not know that "build" is a build.
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

// The sandbox a step runs in, named from the job and the step. One rule, in
// one place, because Cancel has to be able to derive these names from a job
// it was handed rather than from bookkeeping it hopes is up to date.
std::string SandboxName(const std::string &job_id,
                        const std::string &step_name);

// The placeholders a step's argv and environment may carry, which only the
// engine running it can resolve. The list is closed on purpose: a job that
// spells one wrong gets it through verbatim rather than silently blank.
//
//   {{scratch}}       the writable directory a step may collect files from
//   {{port_file}}     where a step should publish the port it bound
//   {{peer:<name>}}   the address another step of this phase is reachable at
inline constexpr char kScratchPlaceholder[] = "{{scratch}}";
inline constexpr char kPortFilePlaceholder[] = "{{port_file}}";

// |step| with every occurrence of each key in |replacements| replaced, in its
// argv and in its environment. Applied to both because a graded run names its
// report path in the environment and a bot names its peer in argv, and an
// engine that substituted only one of them would work until it didn't.
proto::Step Substituted(const proto::Step &step,
                        const std::map<std::string, std::string> &replacements);

// Sets |status| and returns false.
bool Fail(proto::Status *status, proto::Status::Code code,
          const std::string &message);

// |override_with| if it sets anything, else |base|: never field by field, since
// half-overridden isolation reads as tight and is not.
proto::Isolation Merge(const proto::Isolation &base,
                       const proto::Isolation &override_with);

// Appends a started step's result, its output read back from
// <log_dir>/<name>.{out,err}.
proto::StepResult *AddStepResult(proto::PhaseResult *phase,
                                 const std::string &name,
                                 const std::filesystem::path &log_dir);

class Engine {
 public:
  virtual ~Engine() = default;

  virtual Capabilities capabilities() const = 0;

  // Runs |job| to completion. Called from the caller's own thread; safe to
  // call concurrently for jobs with different ids.
  virtual proto::JobResult Run(const proto::Job &job, Observer *observer) = 0;

  // Aborts |job_id| if this engine is running it. Called from another thread
  // while Run is in flight, so it must be safe against the job finishing
  // concurrently -- killing what has already exited is a no-op, and that is
  // the race to design for rather than lock against.
  virtual void Cancel(const std::string &job_id) = 0;
};

}  // namespace sandbox_exec

#endif  // GAME_ARENA_GAME_ARENA_SANDBOX_EXEC_ENGINE_H
