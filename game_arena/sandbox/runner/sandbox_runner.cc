#include "game_arena/sandbox/runner/sandbox_runner.h"

#include <filesystem>
#include <string>
#include <system_error>
#include <utility>

#include "absl/log/log.h"
#include "game_arena/sandbox/common/docker.h"
#include "game_arena/sandbox/exec/sandbox_job.pb.h"

namespace sandbox_runner {

namespace sx = sandbox_exec::proto;

namespace {

// Registers a run for the length of its request, so a duplicate id is
// refused. Released however the handler leaves.
struct Registration {
  SandboxRunnerService *service;
  std::map<std::string, std::string> *active;
  std::mutex *mutex;
  std::string id;
  ~Registration() {
    std::lock_guard<std::mutex> lock(*mutex);
    active->erase(id);
  }
};

}  // namespace

bool IsSafePatchPath(const std::string &path) {
  if (path.empty() || path.front() == '/') {
    return false;
  }
  for (const std::filesystem::path &part : std::filesystem::path(path)) {
    if (part == ".." || part == ".") {
      return false;
    }
  }
  return true;
}

std::string ContainerName(const std::string &id) {
  return "sbr-" + sandbox_common::SanitizeContainerName(id);
}

SandboxRunnerService::SandboxRunnerService(SandboxRunnerConfig config,
                                           sandbox_exec::Engine *engine)
    : config_(std::move(config)), engine_(engine) {}

grpc::Status SandboxRunnerService::Run(grpc::ServerContext *context,
                               const proto::RunRequest *request,
                               proto::RunResponse *response) {
  (void)context;
  if (request->id().empty()) {
    return {grpc::StatusCode::INVALID_ARGUMENT, "id is required"};
  }
  if (request->argv().empty()) {
    return {grpc::StatusCode::INVALID_ARGUMENT, "argv is required"};
  }
  for (const sx::StagedFile &file : request->files()) {
    if (!IsSafePatchPath(file.path())) {
      return {grpc::StatusCode::INVALID_ARGUMENT,
              "unsafe file path: " + file.path()};
    }
  }

  const std::string container = ContainerName(request->id());
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!active_.emplace(request->id(), container).second) {
      return {grpc::StatusCode::ALREADY_EXISTS,
              "a run is already in flight for id '" + request->id() + "'"};
    }
  }
  const Registration registration{this, &active_, &mutex_, request->id()};

  const std::filesystem::path scratch = config_.work_dir / container;
  std::error_code ec;
  std::filesystem::remove_all(scratch, ec);

  sx::Job job;
  job.set_id(container);
  job.set_log_dir((scratch / "logs").string());

  sx::Workspace *workspace = job.mutable_workspace();
  workspace->set_tree_dir(config_.repo_dir.string());
  workspace->set_staging_dir((scratch / "patches").string());
  workspace->set_sandbox_work_dir(sandbox_common::kWorkspace);
  // Staged files are copied onto the workspace rather than applied: what a
  // person hands this tool is a tree of files, not a diff.
  workspace->set_patch(sx::Workspace::PATCH_COPY_IN_ENTRYPOINT);
  for (const sx::StagedFile &file : request->files()) {
    *workspace->add_staged_files() = file;
  }

  // The same boundary the fleet worker has: nothing to mount, nothing to be
  // privileged for.
  sx::Isolation *isolation = job.mutable_isolation();
  isolation->set_image(config_.docker_image);
  isolation->set_memory_limit_mb(config_.memory_limit_mb);
  isolation->set_cpus(config_.cpus);
  isolation->set_pids_limit(config_.pids_limit);
  isolation->set_run_as_user(config_.run_as_user);
  sx::Tmpfs *tmpfs = isolation->add_tmpfs();
  tmpfs->set_target("/tmp");
  tmpfs->set_options("exec");

  sx::Phase *phase = job.add_phases();
  phase->set_name("run");
  sx::Step *step = phase->mutable_foreground();
  // Unnamed, so the sandbox is named exactly after the job -- which is what
  // this tool's container names have always looked like.
  step->set_applies_patches(true);
  step->set_timeout_s(request->timeout_s() > 0
                          ? request->timeout_s()
                          : static_cast<int>(config_.timeout.count()));
  for (const sx::Token &token : request->argv()) {
    *step->add_argv() = token;
  }
  for (const auto &[key, value] : request->env()) {
    (*step->mutable_env())[key] = value;
  }

  const sx::JobResult result = engine_->Run(job, nullptr);
  std::filesystem::remove_all(scratch, ec);

  // Always reported, even when the step ran: a cancelled run has both a
  // status worth saying and an exit code worth passing on.
  *response->mutable_status() = result.status();
  if (result.status().code() != sx::Status::OK) {
    LOG(WARNING) << "run " << request->id() << ": "
                 << result.status().message();
  }
  if (result.phases_size() > 0 && result.phases(0).steps_size() > 0) {
    const sx::StepResult &step_result = result.phases(0).steps(0);
    response->set_stdout(step_result.stdout());
    response->set_stderr(step_result.stderr());
    response->set_exit_code(step_result.exit_code());
    response->set_timed_out(step_result.timed_out());
    if (step_result.timed_out()) {
      response->set_stderr(step_result.stderr() +
                           "\n[sandbox_runner] killed: timeout after " +
                           std::to_string(step_result.timeout_s()) + "s\n");
    }
  } else {
    // Nothing ran at all: the workspace, the image or the daemon.
    response->set_exit_code(1);
  }
  return grpc::Status::OK;
}

grpc::Status SandboxRunnerService::Kill(grpc::ServerContext *context,
                                const proto::KillRequest *request,
                                proto::KillResponse *response) {
  (void)context;
  (void)response;
  std::string job_id;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = active_.find(request->id());
    if (it == active_.end()) {
      return {grpc::StatusCode::NOT_FOUND,
              "no run in flight for id '" + request->id() + "'"};
    }
    job_id = it->second;
  }
  engine_->Cancel(job_id);
  return grpc::Status::OK;
}

}  // namespace sandbox_runner
