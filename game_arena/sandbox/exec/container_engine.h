#ifndef GAME_ARENA_GAME_ARENA_SANDBOX_EXEC_CONTAINER_ENGINE_H
#define GAME_ARENA_GAME_ARENA_SANDBOX_EXEC_CONTAINER_ENGINE_H

// Runs a job's steps in throwaway containers.
//
// The engine that is actually a boundary. Per job: export the tree and load
// it, with the staged files, into per-job volumes through the daemon. Per
// phase: create the phase's private bridge if any step asks for one, reap any
// sandbox left behind under the names it is about to use, start the
// background steps detached, run the foreground step, then drain and remove
// everything. At the end, the job's volumes go too.
//
// Nothing is bind-mounted unless a job asks for it (Mount::BIND). That is
// what makes a worker "anything with a docker socket": the daemon never has
// to see this process's filesystem, so it can be behind docker-outside-of-
// docker or DOCKER_HOST, and no host has to mount an overlay for anyone.
//
// What the isolation rests on is stated in exec/isolation.h and applied to
// every container without exception -- there is no path through this file
// that builds a `docker run` without IsolationArgs.
//
// Container and volume names are derived, never remembered: SandboxName(job,
// step). That is what lets Cancel reach a job's containers from the job
// itself rather than from bookkeeping that might be a step behind.

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

  std::string name() const override { return "container"; }
  Capabilities capabilities() const override {
    return Capabilities{/*isolates=*/true, /*shared_network=*/true,
                        /*stable_peer_names=*/true};
  }

  bool Prepare(const proto::Workspace &prototype, int lanes,
               std::string *error) override;
  proto::JobResult Run(const proto::Job &job, Observer *observer) override;
  void Cancel(const std::string &job_id) override;

 private:
  // Everything a Cancel needs to know about a job in flight. Names, not
  // handles: a container is stopped by name, so the engine never has to hold
  // anything that could go stale.
  struct InFlight {
    std::vector<std::string> container_names;
    std::vector<std::string> network_names;
  };

  // Creates the job's volumes and fills the workspace and patch volumes from
  // the exported tree and the staging directory.
  bool LoadWorkspace(const proto::Job &job, proto::Status *status);
  void RemoveVolumes(const proto::Job &job);

  bool RunPhase(const proto::Job &job, const proto::Phase &phase,
                Observer *observer, proto::PhaseResult *result,
                proto::Status *status);

  const ContainerEngineConfig config_;
  std::mutex mutex_;
  std::map<std::string, InFlight> in_flight_;
  std::set<std::string> cancelled_;
};

}  // namespace sandbox_exec

#endif  // GAME_ARENA_GAME_ARENA_SANDBOX_EXEC_CONTAINER_ENGINE_H
