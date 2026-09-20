// Standalone sandbox runner server: `bazel run //<target>` per request,
// inside a throwaway docker container.
/*
bazel run //game_arena/sandbox/runner:sandbox_runner -- \
  --port=50052 --docker_image=nim-sandbox:1 \
  --repo_dir=/large_nfs/game-mcts --timeout_s=1800
*/
//
// Each run gets its own copy of --repo_dir, loaded into a docker volume
// through the daemon, so builds and patches never touch the host checkout and
// the daemon never has to see it. That is what lets this run unchanged from a
// container driving the host's docker (docker-outside-of-docker) or against a
// remote DOCKER_HOST. Requests carry [path, content] files that are copied
// over the tree before bazel runs at its root. The request's identifier is a
// token: passing it to the Kill RPC aborts the run mid-flight, and runs
// exceeding --timeout_s are killed by the server.

#include <grpcpp/grpcpp.h>

#include <chrono>
#include <condition_variable>
#include <csignal>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <system_error>

#include "absl/flags/flag.h"
#include "absl/flags/parse.h"
#include "absl/log/globals.h"
#include "absl/log/initialize.h"
#include "absl/log/log.h"
#include "game_arena/sandbox/exec/container_engine.h"
#include "game_arena/sandbox/runner/sandbox_runner.h"

ABSL_FLAG(int, port, 50052, "Port for the SandboxService gRPC server");
ABSL_FLAG(std::string, docker_image, "",
          "Image runs execute in; must contain bazel (required)");
ABSL_FLAG(std::string, repo_dir, "",
          "The tree each run gets a copy of (required). Read by this process "
          "and never mounted, so the daemon need not see it");
ABSL_FLAG(std::string, work_dir, "/tmp/sandbox_runner",
          "Scratch for staged files and captured output");
ABSL_FLAG(int, timeout_s, 1800,
          "Wall-clock limit per run; the container is killed when it fires. "
          "0 disables the limit");
ABSL_FLAG(int, memory_limit_mb, 0, "cgroup memory cap; 0 disables it");
ABSL_FLAG(double, cpus, 0.0, "cgroup CPU cap; 0 disables it");
ABSL_FLAG(int, pids_limit, 0, "cgroup pid cap; 0 disables it");
ABSL_FLAG(std::string, run_as_user, "",
          "Run as this user inside the sandbox, e.g. \"1000:1000\". Empty "
          "leaves the image's default, which for most images is root");
ABSL_FLAG(std::string, docker, "docker", "docker binary");

namespace {

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

int main(int argc, char **argv) {
  absl::ParseCommandLine(argc, argv);
  absl::InitializeLog();
  absl::SetStderrThreshold(absl::LogSeverityAtLeast::kInfo);

  if (absl::GetFlag(FLAGS_docker_image).empty()) {
    LOG(ERROR) << "Missing required --docker_image=<image>";
    return 2;
  }

  sandbox_runner::SandboxRunnerConfig config;
  config.docker_image = absl::GetFlag(FLAGS_docker_image);
  config.repo_dir = absl::GetFlag(FLAGS_repo_dir);
  config.work_dir = absl::GetFlag(FLAGS_work_dir);
  config.timeout = std::chrono::seconds(absl::GetFlag(FLAGS_timeout_s));
  config.memory_limit_mb = absl::GetFlag(FLAGS_memory_limit_mb);
  config.cpus = absl::GetFlag(FLAGS_cpus);
  config.pids_limit = absl::GetFlag(FLAGS_pids_limit);
  config.run_as_user = absl::GetFlag(FLAGS_run_as_user);

  if (config.repo_dir.empty()) {
    LOG(ERROR) << "Missing required --repo_dir=<tree>";
    return 2;
  }
  if (!std::filesystem::is_directory(config.repo_dir)) {
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
  const std::string lower_dir_note = ", tree " + config.repo_dir.string();

  sandbox_exec::ContainerEngineConfig engine_config;
  engine_config.docker = absl::GetFlag(FLAGS_docker);
  sandbox_exec::ContainerEngine engine(engine_config);
  sandbox_runner::SandboxRunnerService service(std::move(config), &engine);

  grpc::ServerBuilder builder;
  builder.AddListeningPort(
      "0.0.0.0:" + std::to_string(absl::GetFlag(FLAGS_port)),
      grpc::InsecureServerCredentials());
  builder.RegisterService(&service);
  std::unique_ptr<grpc::Server> server = builder.BuildAndStart();
  if (!server) {
    LOG(ERROR) << "Cannot bind gRPC port " << absl::GetFlag(FLAGS_port);
    return 1;
  }

  std::signal(SIGINT, OnShutdownSignal);
  std::signal(SIGTERM, OnShutdownSignal);

  LOG(INFO) << "Sandbox runner on :" << absl::GetFlag(FLAGS_port) << ", image "
            << absl::GetFlag(FLAGS_docker_image) << lower_dir_note
            << ", timeout " << absl::GetFlag(FLAGS_timeout_s) << "s";
  WaitForShutdownSignal();
  LOG(INFO) << "Shutting down";
  server->Shutdown();
  return 0;
}
