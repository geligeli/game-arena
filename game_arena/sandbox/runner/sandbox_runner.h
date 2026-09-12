#ifndef GAME_ARENA_GAME_ARENA_SANDBOX_RUNNER_SANDBOX_RUNNER_H
#define GAME_ARENA_GAME_ARENA_SANDBOX_RUNNER_SANDBOX_RUNNER_H

// Driving the sandbox engine by hand, over RPC.
//
// A development tool, and now a thin one: the container lifecycle, the
// overlay, the timeout handling and the kill all live in
// //game_arena/sandbox/exec, which the fleet worker runs on too. What is left
// here is the request's own concerns -- validating what a caller sent, naming
// the run, and refusing a duplicate id.
//
// That sharing is the point. Before it, this tool and the worker each built
// their own `docker run`, and only one of them was hardened: this one passed
// --cap-add SYS_ADMIN and no network restriction, while the worker dropped
// every capability. Two container launchers, one boundary.

#include <grpcpp/grpcpp.h>

#include <chrono>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>

#include "game_arena/sandbox/exec/engine.h"
#include "game_arena/sandbox/runner/sandbox_service.grpc.pb.h"

namespace sandbox_runner {

struct SandboxRunnerConfig {
  // The image every sandbox starts from. Must contain bazel, a toolchain and
  // /bin/sh.
  std::string docker_image;
  // The overlay's lower dir, mounted read-only. Empty means the image carries
  // the repository and there is nothing to overlay.
  std::filesystem::path repo_dir;
  std::filesystem::path work_dir = "/tmp/sandbox_runner";
  std::chrono::seconds timeout{1800};

  // Under docker-outside-of-docker the daemon resolves bind-mount sources on
  // its own filesystem, not this process's. These say what it will see.
  std::filesystem::path host_repo_dir;
  std::filesystem::path host_work_dir;

  // Assemble the overlay on the host rather than in the sandbox. Off by
  // default here, unlike the fleet worker: a development box is often not
  // root, and the in-sandbox form only needs CAP_SYS_ADMIN. Turning it on
  // gets the same boundary the fleet has.
  bool host_overlay = false;
  int memory_limit_mb = 0;
  double cpus = 0.0;
  int pids_limit = 0;
  std::string run_as_user;

  auto MountRepoDir() const -> const std::filesystem::path & {
    return host_repo_dir.empty() ? repo_dir : host_repo_dir;
  }
  auto MountWorkDir() const -> const std::filesystem::path & {
    return host_work_dir.empty() ? work_dir : host_work_dir;
  }
};

// True when |path| is safe to stage: relative, and free of any ".." or "."
// component. The engine checks this too; here it is the difference between a
// clear error and a confusing one, because this server's callers are people.
auto IsSafePatchPath(const std::string &path) -> bool;

// The sandbox a run of |id| gets. Its own function because the CLI's Kill
// needs to be able to name it.
auto ContainerName(const std::string &id) -> std::string;

class SandboxRunnerService final : public proto::SandboxService::Service {
 public:
  SandboxRunnerService(SandboxRunnerConfig config,
                       sandbox_exec::Engine *engine);

  auto Run(grpc::ServerContext *context, const proto::RunRequest *request,
           proto::RunResponse *response) -> grpc::Status override;

  auto Kill(grpc::ServerContext *context, const proto::KillRequest *request,
            proto::KillResponse *response) -> grpc::Status override;

 private:
  const SandboxRunnerConfig config_;
  sandbox_exec::Engine *const engine_;

  std::mutex mutex_;
  // The runs in flight, so a duplicate id is refused rather than raced. The
  // engine owns aborting them; this is only what it may be asked about.
  std::map<std::string, std::string> active_;
};

}  // namespace sandbox_runner

#endif  // GAME_ARENA_GAME_ARENA_SANDBOX_RUNNER_SANDBOX_RUNNER_H
