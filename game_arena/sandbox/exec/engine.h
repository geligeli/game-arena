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

#include <functional>
#include <string>

#include "game_arena/sandbox/exec/sandbox_job.pb.h"

namespace sandbox_exec {

// What an engine can promise, so a caller can refuse a job this engine cannot
// contain without having to ask what the engine is called.
struct Capabilities {
  // A security boundary, not merely resource limits. A caller that runs
  // submitted code which may execute at build time must require this.
  bool isolates = false;
  // The steps of a phase can reach each other.
  bool shared_network = false;
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
  virtual void OnStepStarted(const std::string &job_id,
                             const std::string &phase,
                             const std::string &step) {
    (void)job_id;
    (void)phase;
    (void)step;
  }
};

// The sandbox a step runs in, named from the job and the step. One rule, in
// one place, because Cancel has to be able to derive these names from a job
// it was handed rather than from bookkeeping it hopes is up to date.
auto SandboxName(const std::string &job_id,
                 const std::string &step_name) -> std::string;

class Engine {
 public:
  virtual ~Engine() = default;

  virtual auto name() const -> std::string = 0;
  virtual auto capabilities() const -> Capabilities = 0;

  // Prepares what |lanes| concurrent jobs will need, so the first job does
  // not pay for it: one clone per lane, and the directories a bind mount
  // would otherwise conjure up empty. |prototype| carries the paths with
  // "{lane}" still in them.
  virtual auto Prepare(const proto::Workspace &prototype, int lanes,
                       std::string *error) -> bool {
    (void)prototype;
    (void)lanes;
    (void)error;
    return true;
  }

  // Runs |job| to completion. Called from the caller's own thread; safe to
  // call concurrently for jobs with different ids.
  virtual auto Run(const proto::Job &job,
                   Observer *observer) -> proto::JobResult = 0;

  // Aborts |job_id| if this engine is running it. Called from another thread
  // while Run is in flight, so it must be safe against the job finishing
  // concurrently -- killing what has already exited is a no-op, and that is
  // the race to design for rather than lock against.
  virtual void Cancel(const std::string &job_id) = 0;
};

}  // namespace sandbox_exec

#endif  // GAME_ARENA_GAME_ARENA_SANDBOX_EXEC_ENGINE_H
