#include "game_arena/sandbox/runner/sandbox_runner.h"

#include <utility>
#include <vector>

#include "absl/log/log.h"
#include "game_arena/common/process/process.h"
#include "game_arena/sandbox/common/docker.h"
#include "game_arena/sandbox/common/files.h"

namespace sandbox_runner {

namespace {

namespace proto = tournament_broker::proto;

// Exit code reported when the server-side timeout fired, matching the
// convention `timeout(1)` uses.
constexpr int kTimeoutExitCode = 124;

// Lays the request's patches out under <scratch>/patches so they can be
// mounted into the container as a directory.
auto WritePatches(const std::filesystem::path &patch_dir,
                  const proto::RunSandboxRequest &request,
                  std::string *error) -> bool {
  for (const auto &[path, content] : request.patches()) {
    if (!sandbox_common::WriteFile(patch_dir / path, content, error)) {
      *error = "cannot stage patch '" + path + "': " + *error;
      return false;
    }
  }
  return true;
}

// The script /bin/sh runs as the container's entrypoint. It assembles the
// writable workspace, drops the patches in, then replaces itself with bazel
// so signals (docker kill, signal proxying) reach the build directly.
auto BuildContainerScript(const SandboxRunnerConfig &config,
                          const proto::RunSandboxRequest &request)
    -> std::string {
  std::string script = "set -eu\n";
  // Without a --repo_dir there is no lower dir to overlay: the image carries
  // the repository, and only the scratch/HOME setup is needed.
  script += config.repo_dir.empty() ? sandbox_common::ScratchSetupScript()
                                    : sandbox_common::OverlayMountScript();
  script += "cp -a ";
  script += sandbox_common::kPatchMount;
  script += "/. ";
  script += sandbox_common::kWorkspace;
  script += "/\ncd ";
  script += sandbox_common::kWorkspace;
  script += "\nexec bazel run ";
  script += sandbox_common::ShellQuote(request.bazel_target());
  script += " --";
  for (const std::string &arg : request.args()) {
    script += " ";
    script += sandbox_common::ShellQuote(arg);
  }
  script += "\n";
  return script;
}

}  // namespace

auto IsSafePatchPath(const std::string &path) -> bool {
  if (path.empty()) {
    return false;
  }
  const std::filesystem::path p(path);
  if (p.is_absolute()) {
    return false;
  }
  for (const auto &component : p) {
    if (component == "." || component == "..") {
      return false;
    }
  }
  return true;
}

auto ContainerName(const std::string &identifier) -> std::string {
  return "sbr-" + sandbox_common::SanitizeContainerName(identifier);
}

SandboxRunnerService::SandboxRunnerService(SandboxRunnerConfig config)
    : config_(std::move(config)) {}

auto SandboxRunnerService::Run(
    grpc::ServerContext * /*context*/, const proto::RunSandboxRequest *request,
    proto::RunSandboxResponse *response) -> grpc::Status {
  if (request->identifier().empty() || request->bazel_target().empty()) {
    return {grpc::StatusCode::INVALID_ARGUMENT,
            "identifier and bazel_target are required"};
  }
  for (const auto &[path, unused] : request->patches()) {
    if (!IsSafePatchPath(path)) {
      return {grpc::StatusCode::INVALID_ARGUMENT,
              "patch path '" + path + "' escapes the repository"};
    }
  }

  const std::string container = ContainerName(request->identifier());
  {
    std::lock_guard lock(mutex_);
    // An identifier is a single-use token: a second Run with the same one
    // would race the first for its container name and scratch directory.
    if (active_.contains(request->identifier())) {
      return {grpc::StatusCode::ALREADY_EXISTS, "a run with identifier '" +
                                                    request->identifier() +
                                                    "' is already in flight"};
    }
    active_[request->identifier()] = container;
  }
  const struct Unregister {
    SandboxRunnerService *self;
    std::string identifier;
    ~Unregister() {
      std::lock_guard lock(self->mutex_);
      self->active_.erase(identifier);
    }
  } unregister{this, request->identifier()};

  const std::filesystem::path scratch = config_.work_dir / container;
  const std::filesystem::path patch_dir = scratch / "patches";
  // Same directory, named as the docker daemon sees it.
  const std::filesystem::path mount_patch_dir =
      config_.MountWorkDir() / container / "patches";
  std::error_code ec;
  std::filesystem::remove_all(scratch, ec);
  std::filesystem::create_directories(patch_dir, ec);
  if (ec) {
    return {
        grpc::StatusCode::INTERNAL,
        "cannot prepare scratch dir " + scratch.string() + ": " + ec.message()};
  }
  std::string error;
  if (!WritePatches(patch_dir, *request, &error)) {
    return {grpc::StatusCode::INTERNAL, error};
  }

  std::vector<std::string> mounts = {
      sandbox_common::BindMount(mount_patch_dir, sandbox_common::kPatchMount,
                                true),
  };
  if (!config_.repo_dir.empty()) {
    mounts.push_back(sandbox_common::BindMount(
        config_.MountRepoDir(), sandbox_common::kLowerMount, true));
  }
  // The overlay mount inside the container needs this one capability;
  // nothing else is granted.
  const std::vector<std::string> args = sandbox_common::DockerRunArgs({
      /*name=*/container,
      /*image=*/config_.docker_image,
      /*script=*/BuildContainerScript(config_, *request),
      /*rm=*/true,
      /*detached=*/false,
      /*network=*/"",
      /*extra_args=*/{"--cap-add", "SYS_ADMIN"},
      /*mounts=*/mounts,
  });

  LOG(INFO) << "run " << request->identifier() << ": " << config_.docker_image
            << " bazel run " << request->bazel_target() << " ("
            << request->patches_size() << " patch file(s), timeout "
            << config_.timeout.count() << "s)";

  const std::filesystem::path stdout_path = scratch / "run.out";
  const std::filesystem::path stderr_path = scratch / "run.err";
  process::RunOptions options;
  options.stdout_path = stdout_path;
  options.stderr_path = stderr_path;
  options.timeout = config_.timeout;
  const process::RunResult result =
      process::RunCommand(config_.docker, args, options);

  if (!result.started) {
    return {grpc::StatusCode::INTERNAL,
            "cannot run docker ('" + config_.docker + "' not found)"};
  }

  if (result.timed_out) {
    // The timeout killed the docker *client*; the container itself is managed
    // by the daemon and would otherwise keep running.
    sandbox_common::KillContainer(config_.docker, container);
    response->set_exit_code(kTimeoutExitCode);
  } else {
    response->set_exit_code(result.exit_code);
  }
  response->set_stdout(sandbox_common::ReadFile(stdout_path));
  response->set_stderr(sandbox_common::ReadFile(stderr_path));
  if (result.timed_out) {
    *response->mutable_stderr() += "\n[sandbox_runner] killed: timeout after " +
                                   std::to_string(config_.timeout.count()) +
                                   "s\n";
  }

  std::filesystem::remove_all(scratch, ec);
  LOG(INFO) << "run " << request->identifier() << ": exit "
            << response->exit_code() << (result.timed_out ? " (timeout)" : "");
  return grpc::Status::OK;
}

auto SandboxRunnerService::Kill(
    grpc::ServerContext * /*context*/, const proto::KillRequest *request,
    proto::KillResponse * /*response*/) -> grpc::Status {
  std::string container;
  {
    std::lock_guard lock(mutex_);
    const auto it = active_.find(request->identifier());
    if (it == active_.end()) {
      return {
          grpc::StatusCode::NOT_FOUND,
          "no run in flight with identifier '" + request->identifier() + "'"};
    }
    container = it->second;
  }

  // docker kill stops the container; the Run handler's `docker run` client
  // exits with it, so the blocked Run RPC returns on its own.
  const process::RunResult result =
      sandbox_common::KillContainer(config_.docker, container);
  if (!result.started) {
    return {grpc::StatusCode::INTERNAL,
            "cannot run docker ('" + config_.docker + "' not found)"};
  }
  // A nonzero exit usually means the run finished between the lookup and the
  // kill -- which is exactly the state the caller asked for, so it is not an
  // error.
  LOG(INFO) << "kill " << request->identifier() << " (" << container
            << "): docker kill exit " << result.exit_code;
  return grpc::Status::OK;
}

}  // namespace sandbox_runner
