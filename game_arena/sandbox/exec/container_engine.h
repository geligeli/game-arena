#ifndef GAME_ARENA_GAME_ARENA_SANDBOX_EXEC_CONTAINER_ENGINE_H
#define GAME_ARENA_GAME_ARENA_SANDBOX_EXEC_CONTAINER_ENGINE_H

// Each step in a throwaway container. Every `docker run` gets IsolationArgs,
// and nothing is bind-mounted unless the job asks for it (Mount::BIND).

#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "game_arena/sandbox/exec/engine.h"
#include "game_arena/sandbox/exec/sandbox_job.pb.h"

namespace sandbox_exec {

struct ContainerEngineConfig {
  std::string docker = "docker";  // tests point this at a fake
};

class ContainerEngine final : public Engine {
 public:
  explicit ContainerEngine(ContainerEngineConfig config);

  Capabilities capabilities() const override {
    return Capabilities{/*isolates=*/true, /*stable_peer_names=*/true};
  }

  proto::JobResult Run(const proto::Job &job, Observer *observer) override;
  void Cancel(const std::string &job_id) override;

 private:
  struct InFlight {
    std::vector<std::string> container_names;
    std::vector<std::string> network_names;
  };

  // Creates the job's volumes, copies the staged files in and chowns them.
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
