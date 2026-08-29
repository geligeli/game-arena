#include "game_mcts/tournament_server/sandbox_runner/sandbox_runner.h"

#include <cctype>
#include <fstream>
#include <system_error>
#include <utility>
#include <vector>

#include "absl/log/log.h"
#include "game_mcts/cpp/process/process.h"

namespace sandbox_runner {

namespace {

namespace proto = tournament_broker::proto;

// Fixed mount points inside the container.
constexpr char kLowerMount[] = "/repo_lower";   // read-only --repo_dir
constexpr char kPatchMount[] = "/patches";      // read-only patch files
constexpr char kWorkspace[] = "/workspace";     // overlay merge, repo root
constexpr char kScratch[] = "/sandbox";         // overlay upper/work, HOME

// Exit code reported when the server-side timeout fired, matching the
// convention `timeout(1)` uses.
constexpr int kTimeoutExitCode = 124;

// A read-only bind mount in `docker run --mount` syntax. The long form is
// deliberate: `-v` creates |source| as an empty directory when it does not
// exist, which under docker-outside-of-docker turns a mistyped host path into
// an empty repository or a run that silently drops every patch. --mount fails
// the run instead.
auto ReadOnlyBindMount(const std::filesystem::path &source,
                       const std::string &target) -> std::string {
  return "type=bind,source=" + source.string() + ",target=" + target +
         ",readonly";
}

auto ReadFile(const std::filesystem::path &path) -> std::string {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return {};
  }
  return std::string(std::istreambuf_iterator<char>(in),
                     std::istreambuf_iterator<char>());
}

// Lays the request's patches out under <scratch>/patches so they can be
// mounted into the container as a directory.
auto WritePatches(const std::filesystem::path &patch_dir,
                  const proto::RunSandboxRequest &request,
                  std::string *error) -> bool {
  std::error_code ec;
  for (const auto &[path, content] : request.patches()) {
    const std::filesystem::path file = patch_dir / path;
    std::filesystem::create_directories(file.parent_path(), ec);
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    if (!out) {
      *error = "cannot stage patch '" + path + "'";
      return false;
    }
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
    if (!out) {
      *error = "short write staging patch '" + path + "'";
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
  script += "mkdir -p ";
  script += kScratch;
  script += "/upper ";
  script += kScratch;
  script += "/work ";
  script += kWorkspace;
  script += "\nexport HOME=";
  script += kScratch;
  script += "\n";
  if (!config.repo_dir.empty()) {
    script += "mount -t overlay overlay -o lowerdir=";
    script += kLowerMount;
    script += ",upperdir=";
    script += kScratch;
    script += "/upper,workdir=";
    script += kScratch;
    script += "/work ";
    script += kWorkspace;
    script += "\n";
  }
  script += "cp -a ";
  script += kPatchMount;
  script += "/. ";
  script += kWorkspace;
  script += "/\ncd ";
  script += kWorkspace;
  script += "\nexec bazel run ";
  script += ShellQuote(request.bazel_target());
  script += " --";
  for (const std::string &arg : request.args()) {
    script += " ";
    script += ShellQuote(arg);
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

auto ShellQuote(const std::string &value) -> std::string {
  std::string quoted = "'";
  for (const char c : value) {
    if (c == '\'') {
      quoted += "'\\''";
    } else {
      quoted += c;
    }
  }
  quoted += "'";
  return quoted;
}

auto ContainerName(const std::string &identifier) -> std::string {
  std::string name = "sbr-";
  for (const unsigned char c : identifier) {
    if (std::isalnum(c) != 0 || c == '_' || c == '.' || c == '-') {
      name += static_cast<char>(c);
    } else {
      name += '-';
    }
  }
  return name;
}

SandboxRunnerService::SandboxRunnerService(SandboxRunnerConfig config)
    : config_(std::move(config)) {}

auto SandboxRunnerService::Run(grpc::ServerContext * /*context*/,
                               const proto::RunSandboxRequest *request,
                               proto::RunSandboxResponse *response)
    -> grpc::Status {
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
      return {grpc::StatusCode::ALREADY_EXISTS,
              "a run with identifier '" + request->identifier() +
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
    return {grpc::StatusCode::INTERNAL,
            "cannot prepare scratch dir " + scratch.string() + ": " +
                ec.message()};
  }
  std::string error;
  if (!WritePatches(patch_dir, *request, &error)) {
    return {grpc::StatusCode::INTERNAL, error};
  }

  std::vector<std::string> args = {
      "run", "--rm", "--name", container,
      // The overlay mount inside the container needs this one capability;
      // nothing else is granted.
      "--cap-add", "SYS_ADMIN",
      "--mount", ReadOnlyBindMount(mount_patch_dir, kPatchMount),
  };
  if (!config_.repo_dir.empty()) {
    args.push_back("--mount");
    args.push_back(ReadOnlyBindMount(config_.MountRepoDir(), kLowerMount));
  }
  args.push_back("--entrypoint");
  args.push_back("/bin/sh");
  args.push_back(config_.docker_image);
  args.push_back("-c");
  args.push_back(BuildContainerScript(config_, *request));

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
    process::RunOptions kill_options;
    kill_options.timeout = std::chrono::seconds(60);
    process::RunCommand(config_.docker, {"kill", container}, kill_options);
    response->set_exit_code(kTimeoutExitCode);
  } else {
    response->set_exit_code(result.exit_code);
  }
  response->set_stdout(ReadFile(stdout_path));
  response->set_stderr(ReadFile(stderr_path));
  if (result.timed_out) {
    *response->mutable_stderr() +=
        "\n[sandbox_runner] killed: timeout after " +
        std::to_string(config_.timeout.count()) + "s\n";
  }

  std::filesystem::remove_all(scratch, ec);
  LOG(INFO) << "run " << request->identifier() << ": exit "
            << response->exit_code()
            << (result.timed_out ? " (timeout)" : "");
  return grpc::Status::OK;
}

auto SandboxRunnerService::Kill(grpc::ServerContext * /*context*/,
                                const proto::KillRequest *request,
                                proto::KillResponse * /*response*/)
    -> grpc::Status {
  std::string container;
  {
    std::lock_guard lock(mutex_);
    const auto it = active_.find(request->identifier());
    if (it == active_.end()) {
      return {grpc::StatusCode::NOT_FOUND,
              "no run in flight with identifier '" + request->identifier() +
                  "'"};
    }
    container = it->second;
  }

  // docker kill stops the container; the Run handler's `docker run` client
  // exits with it, so the blocked Run RPC returns on its own.
  process::RunOptions options;
  options.timeout = std::chrono::seconds(60);
  const process::RunResult result =
      process::RunCommand(config_.docker, {"kill", container}, options);
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
