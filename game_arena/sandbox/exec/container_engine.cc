#include "game_arena/sandbox/exec/container_engine.h"

#include <chrono>
#include <filesystem>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "game_arena/common/process/process.h"
#include "game_arena/sandbox/common/docker.h"
#include "game_arena/sandbox/common/files.h"
#include "game_arena/sandbox/common/step.h"
#include "game_arena/sandbox/common/text.h"
#include "game_arena/sandbox/exec/entrypoint.h"
#include "game_arena/sandbox/exec/isolation.h"
#include "game_arena/sandbox/exec/workspace.h"

namespace sandbox_exec {

namespace {

using sandbox_common::ReadFile;
using sandbox_common::TailOf;

constexpr int kDefaultDrainTimeoutS = 60;

auto Merge(const proto::Isolation &base,
           const proto::Isolation &override_with) -> proto::Isolation {
  // A step's isolation replaces the phase's outright rather than merging
  // field by field. Half-overridden isolation is the kind of thing that reads
  // as tight and is not.
  return override_with.ByteSizeLong() > 0 ? override_with : base;
}

auto TimeoutOf(const proto::Step &step) -> std::chrono::seconds {
  return std::chrono::seconds(step.timeout_s());
}

void Fail(proto::Status *status, proto::Status::Code code,
          const std::string &message, const std::string &phase,
          const std::string &step) {
  status->set_code(code);
  status->set_message(message);
  status->set_phase(phase);
  status->set_step(step);
}

}  // namespace

ContainerEngine::ContainerEngine(ContainerEngineConfig config)
    : config_(std::move(config)) {}

auto ContainerEngine::Prepare(const proto::Workspace &prototype, int lanes,
                              std::string *error) -> bool {
  (void)prototype;
  (void)lanes;
  (void)error;
  // Nothing to do that a job's own PrepareWorkspace does not do: the clone is
  // per lane and the lane's paths arrive on the job. Kept as an override
  // point so a caller can still warm a lane up front.
  return true;
}

auto ContainerEngine::Run(const proto::Job &job,
                          Observer *observer) -> proto::JobResult {
  proto::JobResult result;
  proto::Status *status = result.mutable_status();

  if (job.phases().empty()) {
    Fail(status, proto::Status::INVALID_JOB, "a job needs at least one phase",
         "", "");
    return result;
  }

  const std::filesystem::path log_dir(job.log_dir());
  if (!PrepareWorkspace(job.workspace(), log_dir, status)) {
    return result;
  }
  if (observer != nullptr) {
    observer->OnWorkspaceReady(job.id());
  }

  {
    std::lock_guard<std::mutex> lock(mutex_);
    in_flight_[job.id()] = InFlight{};
    cancelled_.erase(job.id());
  }

  for (const proto::Phase &phase : job.phases()) {
    proto::PhaseResult *phase_result = result.add_phases();
    phase_result->set_name(phase.name());
    if (!RunPhase(job, phase, observer, phase_result, status)) {
      break;
    }
  }

  ReleaseWorkspace(job.workspace());
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (cancelled_.erase(job.id()) > 0) {
      // Unconditionally, including over an OK status: a killed step merely
      // exits nonzero, which on its own is indistinguishable from a step that
      // failed on its own merits. A caller polling needs to tell "you
      // replaced it" from "it broke", and only one of those is worth
      // investigating.
      status->set_code(proto::Status::CANCELLED);
      status->set_message("cancelled");
    }
    in_flight_.erase(job.id());
  }
  return result;
}

auto ContainerEngine::RunPhase(const proto::Job &job, const proto::Phase &phase,
                               Observer *observer, proto::PhaseResult *result,
                               proto::Status *status) -> bool {
  const std::filesystem::path log_dir(job.log_dir());
  const proto::Isolation phase_isolation =
      Merge(job.isolation(), phase.isolation());

  // Every container this phase will start, so one list drives both the
  // pre-start reap and the teardown.
  std::vector<const proto::Step *> steps;
  for (const proto::Step &step : phase.background()) {
    steps.push_back(&step);
  }
  steps.push_back(&phase.foreground());

  bool wants_network = false;
  for (const proto::Step *step : steps) {
    wants_network |=
        NeedsPhaseNetwork(Merge(phase_isolation, step->isolation()));
  }
  // One bridge per job, not per phase: a job has at most one phase that needs
  // one, and the name is what a Cancel derives.
  const std::string network = wants_network ? SandboxName(job.id(), "net") : "";

  const auto teardown = [&] {
    for (const proto::Step *step : steps) {
      sandbox_common::RemoveContainer(config_.docker,
                                      SandboxName(job.id(), step->name()));
    }
    if (!network.empty()) {
      sandbox_common::RemoveNetwork(config_.docker, network);
    }
  };
  // A worker killed mid-job leaves containers behind under these exact names;
  // clear them so a redelivered job starts fresh.
  teardown();

  {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = in_flight_.find(job.id());
    if (it != in_flight_.end()) {
      for (const proto::Step *step : steps) {
        it->second.container_names.push_back(
            SandboxName(job.id(), step->name()));
      }
      if (!network.empty()) {
        it->second.network_names.push_back(network);
      }
    }
  }

  if (!network.empty()) {
    const sandbox_common::StepResult made =
        sandbox_common::CreateInternalNetwork(config_.docker, network, log_dir,
                                              phase.name() + "_network");
    if (!made.run.started) {
      Fail(status, proto::Status::TOOL_MISSING,
           "cannot run docker ('" + config_.docker + "' not found)",
           phase.name(), "");
      return false;
    }
    if (made.run.exit_code != 0) {
      Fail(status, proto::Status::NETWORK_FAILED,
           "cannot create the phase network: " +
               TailOf(ReadFile(log_dir / (phase.name() + "_network.err")), 500),
           phase.name(), "");
      teardown();
      return false;
    }
  }

  if (observer != nullptr) {
    observer->OnPhaseStarted(job.id(), phase.name());
  }

  const auto container_args = [&](const proto::Step &step,
                                  bool detached) -> std::vector<std::string> {
    const proto::Isolation isolation = Merge(phase_isolation, step.isolation());
    // Inside a container the scratch dir is always at the same mount point,
    // wherever it came from on the host.
    const proto::Step resolved =
        Substituted(step, {{kScratchPlaceholder, sandbox_common::kScratch}});
    sandbox_common::DockerRunSpec spec;
    spec.name = SandboxName(job.id(), step.name());
    spec.image = isolation.image();
    spec.script = EntrypointScript(job.workspace(), resolved);
    spec.rm = !step.keep_after_exit();
    spec.detached = detached;
    spec.network = NetworkArg(isolation, network);
    spec.extra_args = IsolationArgs(isolation);
    spec.mounts = WorkspaceMounts(job.workspace());
    for (const proto::Mount &mount : step.mounts()) {
      spec.mounts.push_back(sandbox_common::BindMount(
          mount.source(), mount.target(), mount.readonly()));
    }
    return sandbox_common::DockerRunArgs(spec);
  };

  // Background steps first, detached, so the foreground one has something to
  // talk to.
  for (const proto::Step &step : phase.background()) {
    if (observer != nullptr) {
      observer->OnStepStarted(job.id(), phase.name(), step.name());
    }
    process::RunOptions opts;
    opts.timeout = std::chrono::seconds(120);
    opts.stdout_path = log_dir / (step.name() + "_start.out");
    opts.stderr_path = log_dir / (step.name() + "_start.err");
    const process::RunResult started = process::RunCommand(
        config_.docker, container_args(step, /*detached=*/true), opts);
    if (!started.started || started.exit_code != 0) {
      Fail(status, proto::Status::START_FAILED,
           "cannot start " + step.name() + ": " +
               TailOf(ReadFile(opts.stderr_path), 1000),
           phase.name(), step.name());
      teardown();
      return false;
    }
  }

  const proto::Step &foreground = phase.foreground();
  if (observer != nullptr) {
    observer->OnStepStarted(job.id(), phase.name(), foreground.name());
  }
  const sandbox_common::StepResult ran = sandbox_common::RunStep(
      config_.docker, container_args(foreground, /*detached=*/false),
      /*cwd=*/{}, log_dir, foreground.name(), TimeoutOf(foreground));

  proto::StepResult *foreground_result = result->add_steps();
  foreground_result->set_name(foreground.name());
  foreground_result->set_started(ran.run.started);
  foreground_result->set_timeout_s(foreground.timeout_s());
  foreground_result->set_stdout(
      ReadFile(log_dir / (foreground.name() + ".out")));
  foreground_result->set_stderr(
      ReadFile(log_dir / (foreground.name() + ".err")));

  if (!ran.run.started) {
    Fail(status, proto::Status::TOOL_MISSING,
         "cannot run docker ('" + config_.docker + "' not found)", phase.name(),
         foreground.name());
    teardown();
    return false;
  }
  if (ran.run.timed_out) {
    // The timeout killed the docker *client*; the container belongs to the
    // daemon and has to be stopped by name.
    sandbox_common::KillContainer(config_.docker,
                                  SandboxName(job.id(), foreground.name()));
    foreground_result->set_timed_out(true);
    // 124, as timeout(1) reports it, so a numeric-only consumer is not lied
    // to about why this ended.
    foreground_result->set_exit_code(124);
  } else {
    foreground_result->set_exit_code(ran.run.exit_code);
  }

  // Drain the background steps: wait for them to finish counting, then ask
  // for what they printed. They were started with nobody attached to their
  // output, so it has to be asked for.
  const int drain_timeout_s = phase.drain_timeout_s() > 0
                                  ? phase.drain_timeout_s()
                                  : kDefaultDrainTimeoutS;
  for (const proto::Step &step : phase.background()) {
    const std::string container = SandboxName(job.id(), step.name());
    sandbox_common::WaitForContainer(config_.docker, container,
                                     std::chrono::seconds(drain_timeout_s),
                                     log_dir, step.name() + "_wait");
    sandbox_common::ContainerLogs(config_.docker, container, log_dir,
                                  step.name());
    proto::StepResult *background_result = result->add_steps();
    background_result->set_name(step.name());
    background_result->set_started(true);
    background_result->set_stdout(ReadFile(log_dir / (step.name() + ".out")));
    background_result->set_stderr(ReadFile(log_dir / (step.name() + ".err")));
  }

  // Whatever the steps were asked to bring home, read off the scratch dir.
  const std::filesystem::path scratch = ScratchDirOf(job.workspace());
  for (proto::StepResult &step_result : *result->mutable_steps()) {
    const proto::Step *step = nullptr;
    for (const proto::Step *candidate : steps) {
      if (candidate->name() == step_result.name()) {
        step = candidate;
        break;
      }
    }
    if (step == nullptr) {
      continue;
    }
    for (const std::string &file : step->collect_files()) {
      const std::string content = ReadFile(scratch / file);
      if (!content.empty()) {
        (*step_result.mutable_collected())[file] = content;
      }
    }
  }

  teardown();
  return foreground_result->exit_code() == 0 && !foreground_result->timed_out();
}

void ContainerEngine::Cancel(const std::string &job_id) {
  std::vector<std::string> containers;
  std::vector<std::string> networks;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = in_flight_.find(job_id);
    if (it == in_flight_.end()) {
      return;
    }
    cancelled_.insert(job_id);
    containers = it->second.container_names;
    networks = it->second.network_names;
  }
  // Killing something that has already exited is a no-op, and that is the
  // race worth designing for rather than locking against: the job's own
  // thread may be finishing while this runs.
  for (const std::string &container : containers) {
    sandbox_common::KillContainer(config_.docker, container);
  }
  // The network too. Before this engine, a cancelled match left one behind
  // per order: teardown only ran on Run's own exit paths.
  for (const std::string &network : networks) {
    sandbox_common::RemoveNetwork(config_.docker, network);
  }
}

}  // namespace sandbox_exec
