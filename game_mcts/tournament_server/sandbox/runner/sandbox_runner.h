#ifndef GAME_MCTS_TOURNAMENT_SERVER_SANDBOX_RUNNER_SANDBOX_RUNNER_H
#define GAME_MCTS_TOURNAMENT_SERVER_SANDBOX_RUNNER_SANDBOX_RUNNER_H

// Standalone sandbox runner: executes `bazel run //<target>` inside a
// throwaway docker container, behind the SandboxService interface from
// sandbox_runner.proto.
//
// Each Run request gets its own container. When the server was started with
// --repo_dir, that directory is mounted read-only and becomes the lower dir
// of an overlay mounted at /workspace, so the container patches and builds
// against a private copy-on-write view and the host checkout is never
// touched. Patches from the request ([path, content] tuples) are written into
// the merged tree, then `bazel run` executes at its root.
//
// Two filesystems are in play and the config keeps them apart. Paths this
// process reads and writes (work_dir, and the patches under it) are resolved
// here; bind-mount sources are resolved by whatever machine the docker daemon
// runs on. They coincide in the ordinary case, but not when the runner is
// itself containerized and driving the host's daemon over a mounted socket
// (docker-outside-of-docker), where the same tree carries different paths on
// either side. host_repo_dir/host_work_dir supply the daemon's view.
//
// Run blocks until the container exits, the server-side timeout fires, or a
// Kill RPC with the same identifier stops the container mid-flight.

#include <chrono>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>

#include <grpcpp/grpcpp.h>

#include "game_mcts/tournament_server/proto/sandbox_runner.grpc.pb.h"
#include "game_mcts/tournament_server/proto/sandbox_runner.pb.h"

namespace sandbox_runner {

struct SandboxRunnerConfig {
  std::string docker = "docker";
  // Image the containers are started from; must contain bazel.
  std::string docker_image;
  // Mounted read-only as the overlay's lower dir. Empty: /workspace is plain
  // container filesystem, so the image must carry the repository itself.
  std::filesystem::path repo_dir;
  // Scratch for the patch files and captured output of each run, as *this*
  // process resolves it: the patches are written here.
  std::filesystem::path work_dir = "/tmp/sandbox_runner";
  // The same two directories as the *docker daemon* resolves them, for the
  // bind mounts. Empty (the ordinary case): the daemon shares our filesystem
  // and repo_dir/work_dir are handed to it verbatim. Under
  // docker-outside-of-docker they differ, and only these reach docker.
  std::filesystem::path host_repo_dir;
  std::filesystem::path host_work_dir;
  // Wall-clock limit per run; the container is killed when it fires.
  // Zero disables the limit.
  std::chrono::seconds timeout{1800};

  // The bind-mount sources docker is given. Never open these locally: under
  // docker-outside-of-docker they name paths in a filesystem this process
  // cannot see.
  auto MountRepoDir() const -> const std::filesystem::path & {
    return host_repo_dir.empty() ? repo_dir : host_repo_dir;
  }
  auto MountWorkDir() const -> const std::filesystem::path & {
    return host_work_dir.empty() ? work_dir : host_work_dir;
  }
};

// True when |path| is relative and free of '.'/'..' components, so it can be
// safely written under a scratch directory or the in-container workspace.
auto IsSafePatchPath(const std::string &path) -> bool;

// Single-quote escaping for embedding an arbitrary string into the
// container's `/bin/sh -c` script.
auto ShellQuote(const std::string &value) -> std::string;

// Stable docker container name for a run identifier, within docker's
// [a-zA-Z0-9][a-zA-Z0-9_.-]* alphabet. The name doubles as the scratch
// directory name and as the handle Kill uses to stop the container.
auto ContainerName(const std::string &identifier) -> std::string;

class SandboxRunnerService final
    : public tournament_broker::proto::SandboxService::Service {
 public:
  explicit SandboxRunnerService(SandboxRunnerConfig config);

  auto Run(grpc::ServerContext *context,
           const tournament_broker::proto::RunSandboxRequest *request,
           tournament_broker::proto::RunSandboxResponse *response)
      -> grpc::Status override;

  auto Kill(grpc::ServerContext *context,
            const tournament_broker::proto::KillRequest *request,
            tournament_broker::proto::KillResponse *response)
      -> grpc::Status override;

 private:
  SandboxRunnerConfig config_;

  std::mutex mutex_;
  // identifier -> container name for every run in flight; Kill's only view of
  // what can be aborted.
  std::map<std::string, std::string> active_;
};

}  // namespace sandbox_runner

#endif  // GAME_MCTS_TOURNAMENT_SERVER_SANDBOX_RUNNER_SANDBOX_RUNNER_H
