#ifndef GAME_ARENA_GAME_ARENA_SANDBOX_EXEC_CONTAINER_ENGINE_H
#define GAME_ARENA_GAME_ARENA_SANDBOX_EXEC_CONTAINER_ENGINE_H

// Runs a job's steps in throwaway containers.
//
// The engine that is actually a boundary. Per phase: create the phase's
// private bridge if any step asks for one, reap any sandbox left behind under
// the names it is about to use, start the background steps detached, run the
// foreground step, then drain and remove everything.
//
// What the isolation rests on is stated in exec/isolation.h and applied to
// every container without exception -- there is no path through this file
// that builds a `docker run` without IsolationArgs. The overlay is assembled
// on the host (exec/workspace.h), which is what lets these containers run
// with no capabilities at all.
//
// Container names are derived, never remembered: SandboxName(job, step). That
// is what lets Cancel reach a job's containers from the job itself rather
// than from bookkeeping that might be a step behind.

#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "game_arena/sandbox/exec/engine.h"
#include "game_arena/sandbox/exec/sandbox_job.pb.h"

namespace sandbox_exec {

struct ContainerEngineConfig {
  // Configurable for the same reason the git and mount binaries are: so a
  // test can point it somewhere else and assert the exact argv without a
  // daemon.
  std::string docker = "docker";
};

class ContainerEngine final : public Engine {
 public:
  explicit ContainerEngine(ContainerEngineConfig config);

  auto name() const -> std::string override { return "container"; }
  auto capabilities() const -> Capabilities override {
    return Capabilities{/*isolates=*/true, /*shared_network=*/true,
                        /*stable_peer_names=*/true};
  }

  auto Prepare(const proto::Workspace &prototype, int lanes,
               std::string *error) -> bool override;
  auto Run(const proto::Job &job,
           Observer *observer) -> proto::JobResult override;
  void Cancel(const std::string &job_id) override;

 private:
  // Everything a Cancel needs to know about a job in flight. Names, not
  // handles: a container is stopped by name, so the engine never has to hold
  // anything that could go stale.
  struct InFlight {
    std::vector<std::string> container_names;
    std::vector<std::string> network_names;
  };

  auto RunPhase(const proto::Job &job, const proto::Phase &phase,
                Observer *observer, proto::PhaseResult *result,
                proto::Status *status) -> bool;

  const ContainerEngineConfig config_;
  std::mutex mutex_;
  std::map<std::string, InFlight> in_flight_;
  std::set<std::string> cancelled_;
};

}  // namespace sandbox_exec

#endif  // GAME_ARENA_GAME_ARENA_SANDBOX_EXEC_CONTAINER_ENGINE_H
