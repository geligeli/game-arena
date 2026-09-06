// Standalone sandbox runner server: `bazel run //<target>` per request,
// inside a throwaway docker container.
/*
bazel run //game_arena/sandbox/runner:sandbox_runner -- \
  --port=50052 --docker_image=takumi.city/jax-cpu-training:latest \
  --repo_dir=/large_nfs/game-mcts --timeout_s=1800
*/
//
// --repo_dir is mounted read-only into each container and used as the lower
// dir of an overlay, so builds and patches never touch the host checkout.
// Requests carry [path, content] patches that are written into the merged
// tree before bazel runs at its root. The request's identifier is a token:
// passing it to the Kill RPC aborts the run mid-flight, and runs exceeding
// --timeout_s are killed by the server.
//
// Running the server itself inside a container that drives the host's docker
// daemon (docker-outside-of-docker) splits every directory in two: --repo_dir
// and --work_dir stay the paths *this* process resolves, while the daemon
// resolves the bind mounts in the host's tree. Give it that tree with
// --host_repo_dir/--host_work_dir. With this repo's devcontainer, where the
// checkout is bind-mounted from /home/geli/game-mcts:
/*
bazel run //game_arena/sandbox/runner:sandbox_runner -- \
  --port=50052 --docker_image=takumi.city/jax-cpu-training:latest \
  --repo_dir=/large_nfs/game-mcts --host_repo_dir=/home/geli/game-mcts \
  --work_dir=/large_nfs/game-mcts/.sandbox_work \
  --host_work_dir=/home/geli/game-mcts/.sandbox_work
*/
// --work_dir must land somewhere both sides can see -- a path under the
// checkout is the easy choice, since that bind mount already spans them.

#include <chrono>
#include <condition_variable>
#include <csignal>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <system_error>

#include <grpcpp/grpcpp.h>

#include "absl/flags/flag.h"
#include "absl/flags/parse.h"
#include "absl/log/globals.h"
#include "absl/log/initialize.h"
#include "absl/log/log.h"
#include "game_arena/sandbox/runner/sandbox_runner.h"

ABSL_FLAG(int, port, 50052, "Port for the SandboxService gRPC server");
ABSL_FLAG(std::string, docker_image, "",
          "Image runs execute in; must contain bazel (required)");
ABSL_FLAG(std::string, repo_dir, "",
          "Directory mounted read-only into the container as the overlay's "
          "lower dir. Empty: the image must carry the repository itself");
ABSL_FLAG(std::string, work_dir, "/tmp/sandbox_runner",
          "Scratch for staged patches and captured output, as this process "
          "resolves it");
ABSL_FLAG(std::string, host_repo_dir, "",
          "--repo_dir as the docker daemon resolves it. Set this only when "
          "the daemon does not share this process's filesystem, i.e. the "
          "server runs in a container driving the host's docker "
          "(docker-outside-of-docker). Empty: --repo_dir is used verbatim");
ABSL_FLAG(std::string, host_work_dir, "",
          "--work_dir as the docker daemon resolves it; see --host_repo_dir. "
          "It must be the same directory: the patches this server writes "
          "under --work_dir are bind-mounted into the container from here");
ABSL_FLAG(int, timeout_s, 1800,
          "Wall-clock limit per run; the container is killed when it fires. "
          "0 disables the limit");
ABSL_FLAG(std::string, docker, "docker", "docker binary");

namespace {

// Bind-mount sources must be absolute: docker rejects a relative source, and
// `-v` would have quietly read one as a named volume instead.
auto CheckMountSource(const std::string &flag,
                      const std::filesystem::path &path) -> bool {
  if (!path.is_absolute()) {
    LOG(ERROR) << flag << " " << path << " must be an absolute path";
    return false;
  }
  return true;
}

std::mutex g_shutdown_mutex;
std::condition_variable g_shutdown_cv;
bool g_shutdown_requested = false;

// Runs in signal context, so it does the least it can: set a flag and wake the
// main thread, which does the actual shutdown.
extern "C" void OnShutdownSignal(int /*signum*/) {
  {
    std::lock_guard lock(g_shutdown_mutex);
    g_shutdown_requested = true;
  }
  g_shutdown_cv.notify_all();
}

void WaitForShutdownSignal() {
  std::unique_lock lock(g_shutdown_mutex);
  g_shutdown_cv.wait(lock, [] { return g_shutdown_requested; });
}

}  // namespace

auto main(int argc, char **argv) -> int {
  absl::ParseCommandLine(argc, argv);
  absl::InitializeLog();
  absl::SetStderrThreshold(absl::LogSeverityAtLeast::kInfo);

  if (absl::GetFlag(FLAGS_docker_image).empty()) {
    LOG(ERROR) << "Missing required --docker_image=<image>";
    return 2;
  }

  sandbox_runner::SandboxRunnerConfig config;
  config.docker = absl::GetFlag(FLAGS_docker);
  config.docker_image = absl::GetFlag(FLAGS_docker_image);
  config.repo_dir = absl::GetFlag(FLAGS_repo_dir);
  config.work_dir = absl::GetFlag(FLAGS_work_dir);
  config.host_repo_dir = absl::GetFlag(FLAGS_host_repo_dir);
  config.host_work_dir = absl::GetFlag(FLAGS_host_work_dir);
  config.timeout = std::chrono::seconds(absl::GetFlag(FLAGS_timeout_s));

  if (config.repo_dir.empty() && !config.host_repo_dir.empty()) {
    LOG(ERROR) << "--host_repo_dir needs --repo_dir: without the latter no "
                  "lower dir is mounted at all";
    return 2;
  }
  if (!config.repo_dir.empty() &&
      !CheckMountSource("--repo_dir/--host_repo_dir", config.MountRepoDir())) {
    return 2;
  }
  if (!CheckMountSource("--work_dir/--host_work_dir", config.MountWorkDir())) {
    return 2;
  }

  // Only the daemon can resolve a --host_* path, so these checks cover what
  // this process can actually see. Where the two views coincide -- the usual
  // case -- that is also the path docker will mount, and a bad --repo_dir is
  // caught here instead of surfacing as an empty repository inside the
  // container.
  if (!config.repo_dir.empty() && config.host_repo_dir.empty() &&
      !std::filesystem::is_directory(config.repo_dir)) {
    LOG(ERROR) << "--repo_dir " << config.repo_dir << " is not a directory";
    return 1;
  }
  std::error_code ec;
  std::filesystem::create_directories(config.work_dir, ec);
  if (ec) {
    LOG(ERROR) << "Cannot create --work_dir " << config.work_dir << ": "
               << ec.message();
    return 1;
  }
  if (!config.host_repo_dir.empty() || !config.host_work_dir.empty()) {
    LOG(WARNING) << "Docker-side paths (" << config.MountRepoDir() << ", "
                 << config.MountWorkDir()
                 << ") cannot be verified from here; a wrong one fails the "
                    "first run when docker cannot find the bind source";
  }

  // Read off what the startup banner reports before the config is moved out.
  const std::string lower_dir_note =
      config.repo_dir.empty() ? ""
                              : ", lower dir " + config.MountRepoDir().string();

  sandbox_runner::SandboxRunnerService service(std::move(config));

  grpc::ServerBuilder builder;
  builder.AddListeningPort("0.0.0.0:" + std::to_string(absl::GetFlag(FLAGS_port)),
                           grpc::InsecureServerCredentials());
  builder.RegisterService(&service);
  std::unique_ptr<grpc::Server> server = builder.BuildAndStart();
  if (!server) {
    LOG(ERROR) << "Cannot bind gRPC port " << absl::GetFlag(FLAGS_port);
    return 1;
  }

  std::signal(SIGINT, OnShutdownSignal);
  std::signal(SIGTERM, OnShutdownSignal);

  LOG(INFO) << "Sandbox runner on :" << absl::GetFlag(FLAGS_port)
            << ", image " << absl::GetFlag(FLAGS_docker_image)
            << lower_dir_note
            << ", timeout " << absl::GetFlag(FLAGS_timeout_s) << "s";
  WaitForShutdownSignal();
  LOG(INFO) << "Shutting down";
  server->Shutdown();
  return 0;
}
