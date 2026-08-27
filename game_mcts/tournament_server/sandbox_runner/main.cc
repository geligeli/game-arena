// Standalone sandbox runner server: `bazel run //<target>` per request,
// inside a throwaway docker container.
/*
bazel run //game_mcts/tournament_server/sandbox_runner:sandbox_runner --
  --port=50052 --docker_image= takumi.city/jax-cpu-training:latest
  --repo_dir=/path/to/repo --timeout_s=1800
*/
//
// --repo_dir is mounted read-only into each container and used as the lower
// dir of an overlay, so builds and patches never touch the host checkout.
// Requests carry [path, content] patches that are written into the merged
// tree before bazel runs at its root. The request's identifier is a token:
// passing it to the Kill RPC aborts the run mid-flight, and runs exceeding
// --timeout_s are killed by the server.

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
#include "game_mcts/tournament_server/sandbox_runner/sandbox_runner.h"

ABSL_FLAG(int, port, 50052, "Port for the SandboxService gRPC server");
ABSL_FLAG(std::string, docker_image, "",
          "Image runs execute in; must contain bazel (required)");
ABSL_FLAG(std::string, repo_dir, "",
          "Directory mounted read-only into the container as the overlay's "
          "lower dir. Empty: the image must carry the repository itself");
ABSL_FLAG(std::string, work_dir, "/tmp/sandbox_runner",
          "Host-side scratch for staged patches and captured output");
ABSL_FLAG(int, timeout_s, 1800,
          "Wall-clock limit per run; the container is killed when it fires. "
          "0 disables the limit");
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
  config.timeout = std::chrono::seconds(absl::GetFlag(FLAGS_timeout_s));

  if (!config.repo_dir.empty() && !std::filesystem::is_directory(config.repo_dir)) {
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
            << (absl::GetFlag(FLAGS_repo_dir).empty()
                    ? ""
                    : ", lower dir " + absl::GetFlag(FLAGS_repo_dir))
            << ", timeout " << absl::GetFlag(FLAGS_timeout_s) << "s";
  WaitForShutdownSignal();
  LOG(INFO) << "Shutting down";
  server->Shutdown();
  return 0;
}
