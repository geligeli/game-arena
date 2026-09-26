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

proto::Isolation Merge(const proto::Isolation &base,
                       const proto::Isolation &override_with) {
  // A step's isolation replaces the phase's outright rather than merging
  // field by field. Half-overridden isolation is the kind of thing that reads
  // as tight and is not.
  return override_with.ByteSizeLong() > 0 ? override_with : base;
}

std::chrono::seconds TimeoutOf(const proto::Step &step) {
  return std::chrono::seconds(step.timeout_s());
}

// The job's own volumes: the tree, the staged files, and scratch.
std::string WorkspaceVolume(const proto::Job &job) {
  return SandboxName(job.id(), "ws");
}
std::string PatchesVolume(const proto::Job &job) {
  return SandboxName(job.id(), "patches");
}
std::string ScratchVolume(const proto::Job &job) {
  return SandboxName(job.id(), "scratch");
}
std::string LoaderName(const proto::Job &job) {
  return SandboxName(job.id(), "load");
}

// The steps with a scratch volume of their own, and where the loader mounts
// each to hand it to the sandbox's user.
std::vector<const proto::Step *> PrivateScratchSteps(const proto::Job &job) {
  std::vector<const proto::Step *> steps;
  for (const proto::Phase &phase : job.phases()) {
    for (const proto::Step &step : phase.background()) {
      if (step.private_scratch()) {
        steps.push_back(&step);
      }
    }
    if (phase.foreground().private_scratch()) {
      steps.push_back(&phase.foreground());
    }
  }
  return steps;
}
std::string ScratchVolume(const proto::Job &job, const proto::Step &step) {
  return step.private_scratch()
             ? SandboxName(job.id(), step.name() + "-scratch")
             : ScratchVolume(job);
}
std::string LoaderScratchMount(const proto::Step &step) {
  return "/private_scratch/" + step.name();
}

std::vector<std::string> JobVolumes(const proto::Job &job) {
  std::vector<std::string> volumes = {WorkspaceVolume(job), PatchesVolume(job),
                                      ScratchVolume(job)};
  for (const proto::Step *step : PrivateScratchSteps(job)) {
    volumes.push_back(ScratchVolume(job, *step));
  }
  return volumes;
}

std::string MountArg(const proto::Mount &mount) {
  return mount.kind() == proto::Mount::VOLUME
             ? sandbox_common::VolumeMount(mount.source(), mount.target(),
                                           mount.readonly())
             : sandbox_common::BindMount(mount.source(), mount.target(),
                                         mount.readonly());
}

// The `--mount` arguments every step of |job| gets, in order: the tree, the
// scratch dir, then whatever the job asked for.
std::vector<std::string> WorkspaceMounts(const proto::Job &job,
                                         const proto::Step &step) {
  std::vector<std::string> mounts = {
      sandbox_common::VolumeMount(WorkspaceVolume(job),
                                  sandbox_common::kWorkspace, false),
      sandbox_common::VolumeMount(ScratchVolume(job, step),
                                  sandbox_common::kScratch, false)};
  for (const proto::Mount &mount : job.workspace().mounts()) {
    mounts.push_back(MountArg(mount));
  }
  return mounts;
}

// True when a step needs the staged files at /patches.
bool AppliesStagedFiles(const proto::Job &job, const proto::Step &step) {
  return step.applies_patches() &&
         job.workspace().patch() != proto::Workspace::PATCH_NONE &&
         job.workspace().patch() != proto::Workspace::PATCH_HOST;
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

bool ContainerEngine::Prepare(const proto::Workspace &prototype, int lanes,
                              std::string *error) {
  (void)prototype;
  (void)lanes;
  (void)error;
  // Nothing to do that a job's own PrepareWorkspace does not do. Kept as an
  // override point so a caller can still warm a lane up front.
  return true;
}

proto::JobResult ContainerEngine::Run(const proto::Job &job,
                                      Observer *observer) {
  proto::JobResult result;
  proto::Status *status = result.mutable_status();

  if (job.phases().empty()) {
    Fail(status, proto::Status::INVALID_JOB, "a job needs at least one phase",
         "", "");
    return result;
  }

  if (job.isolation().image().empty()) {
    Fail(status, proto::Status::INVALID_JOB,
         "a container job needs an image to load its workspace with", "", "");
    return result;
  }

  const std::filesystem::path log_dir(job.log_dir());
  if (!PrepareWorkspace(job.workspace(), log_dir, status)) {
    return result;
  }
  if (!LoadWorkspace(job, status)) {
    RemoveVolumes(job);
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

  RemoveVolumes(job);
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

bool ContainerEngine::LoadWorkspace(const proto::Job &job,
                                    proto::Status *status) {
  const std::filesystem::path log_dir(job.log_dir());
  const proto::Workspace &ws = job.workspace();

  // A job killed mid-flight leaves its volumes behind under these names;
  // clear them so a redelivered job starts from the tree, not from whatever
  // the last attempt left in it.
  RemoveVolumes(job);
  for (const std::string &volume : JobVolumes(job)) {
    const sandbox_common::StepResult made = sandbox_common::CreateVolume(
        config_.docker, volume, log_dir, "volume_" + volume);
    if (!made.run.started) {
      Fail(status, proto::Status::TOOL_MISSING,
           "cannot run docker ('" + config_.docker + "' not found)", "", "");
      return false;
    }
    if (made.run.exit_code != 0) {
      Fail(status, proto::Status::WORKSPACE_FAILED,
           "cannot create volume " + volume + ": " + TailOf(made.output, 500),
           "", "");
      return false;
    }
  }

  // The loader: a container that exists so the daemon has somewhere to copy
  // into, and that runs once to hand the copied tree to the sandbox's user.
  // Always, root included: the tar keeps the worker's ownership, and a
  // sandbox drops every capability, so even root cannot write into someone
  // else's directory there -- git apply then reports success and lands
  // nothing. Its mounts are the job's volumes -- the persistent ones too, so
  // a fresh one is owned by that user before a step tries to write to it --
  // and never a bind mount, whose ownership is the host's business.
  const std::string user = job.isolation().run_as_user().empty()
                               ? "0:0"
                               : job.isolation().run_as_user();
  std::string script = "chown -R " + user + " " + sandbox_common::kWorkspace +
                       " " + sandbox_common::kPatchMount + " " +
                       sandbox_common::kScratch + "\n";
  for (const proto::Step *step : PrivateScratchSteps(job)) {
    script += "chown " + user + " " + LoaderScratchMount(*step) + "\n";
  }
  for (const proto::Mount &mount : ws.mounts()) {
    if (mount.kind() == proto::Mount::VOLUME) {
      script += "chown " + user + " " + mount.target() + "\n";
    }
  }
  // A read-only mount is not the sandbox's to write, and is usually another
  // step's writable one again.
  for (const proto::Phase &phase : job.phases()) {
    for (const proto::Mount &mount : phase.foreground().mounts()) {
      if (mount.kind() == proto::Mount::VOLUME && !mount.readonly()) {
        script += "chown " + user + " " + mount.target() + "\n";
      }
    }
  }
  sandbox_common::DockerRunSpec spec;
  spec.name = LoaderName(job);
  spec.image = job.isolation().image();
  spec.script = script;
  spec.create = true;
  spec.network = "none";
  spec.mounts = {sandbox_common::VolumeMount(WorkspaceVolume(job),
                                             sandbox_common::kWorkspace, false),
                 sandbox_common::VolumeMount(
                     PatchesVolume(job), sandbox_common::kPatchMount, false),
                 sandbox_common::VolumeMount(ScratchVolume(job),
                                             sandbox_common::kScratch, false)};
  for (const proto::Step *step : PrivateScratchSteps(job)) {
    spec.mounts.push_back(sandbox_common::VolumeMount(
        ScratchVolume(job, *step), LoaderScratchMount(*step), false));
  }
  for (const proto::Mount &mount : ws.mounts()) {
    if (mount.kind() == proto::Mount::VOLUME) {
      spec.mounts.push_back(MountArg(mount));
    }
  }
  for (const proto::Phase &phase : job.phases()) {
    for (const proto::Mount &mount : phase.foreground().mounts()) {
      if (mount.kind() == proto::Mount::VOLUME && !mount.readonly()) {
        spec.mounts.push_back(MountArg(mount));
      }
    }
  }

  const std::string loader = LoaderName(job);
  sandbox_common::RemoveContainer(config_.docker, loader);
  const auto fail_load = [&](const std::string &what,
                             const sandbox_common::StepResult &step) {
    Fail(status, proto::Status::WORKSPACE_FAILED,
         what + ": " + TailOf(step.output, 1000), "", "");
    sandbox_common::RemoveContainer(config_.docker, loader);
    return false;
  };

  const sandbox_common::StepResult created = sandbox_common::RunStep(
      config_.docker, sandbox_common::DockerRunArgs(spec), /*cwd=*/{}, log_dir,
      "load_create", std::chrono::seconds(120));
  if (!created.run.started || created.run.exit_code != 0) {
    return fail_load("cannot create the workspace loader", created);
  }

  if (!ws.tree_dir().empty()) {
    const std::filesystem::path archive = log_dir / "tree.tar";
    if (!ExportTree(ws, archive, log_dir, status)) {
      sandbox_common::RemoveContainer(config_.docker, loader);
      return false;
    }
    // `docker cp -` takes the tar on stdin: the client reads it, the daemon
    // unpacks it, and no path has to be visible to both.
    const sandbox_common::StepResult copied = sandbox_common::RunStep(
        config_.docker, {"cp", "-", loader + ":" + sandbox_common::kWorkspace},
        /*cwd=*/{}, log_dir, "load_tree", std::chrono::seconds(600), 0, {}, {},
        archive);
    std::error_code ec;
    std::filesystem::remove(archive, ec);
    if (!copied.run.started || copied.run.exit_code != 0) {
      return fail_load("cannot load the tree into the sandbox", copied);
    }
  }
  if (!ws.staging_dir().empty() && !ws.staged_files().empty()) {
    const sandbox_common::StepResult copied = sandbox_common::RunStep(
        config_.docker,
        {"cp", ws.staging_dir() + "/.",
         loader + ":" + sandbox_common::kPatchMount},
        /*cwd=*/{}, log_dir, "load_patches", std::chrono::seconds(120));
    if (!copied.run.started || copied.run.exit_code != 0) {
      return fail_load("cannot load the staged files into the sandbox", copied);
    }
  }

  const sandbox_common::StepResult ran = sandbox_common::RunStep(
      config_.docker, {"start", "-a", loader}, /*cwd=*/{}, log_dir,
      "load_start", std::chrono::seconds(600));
  if (!ran.run.started || ran.run.exit_code != 0) {
    return fail_load("the workspace loader failed", ran);
  }
  sandbox_common::RemoveContainer(config_.docker, loader);
  return true;
}

void ContainerEngine::RemoveVolumes(const proto::Job &job) {
  for (const std::string &volume : JobVolumes(job)) {
    sandbox_common::RemoveVolume(config_.docker, volume);
  }
}

bool ContainerEngine::RunPhase(const proto::Job &job, const proto::Phase &phase,
                               Observer *observer, proto::PhaseResult *result,
                               proto::Status *status) {
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
    // Kept when something has to be asked of it afterwards: its logs, or a
    // file it was told to leave in scratch. Teardown removes it either way.
    spec.rm = !step.keep_after_exit() && step.collect_files().empty();
    spec.detached = detached;
    spec.network = NetworkArg(isolation, network);
    spec.extra_args = IsolationArgs(isolation);
    spec.mounts = WorkspaceMounts(job, step);
    if (AppliesStagedFiles(job, step)) {
      spec.mounts.push_back(sandbox_common::VolumeMount(
          PatchesVolume(job), sandbox_common::kPatchMount, true));
    }
    for (const proto::Mount &mount : step.mounts()) {
      spec.mounts.push_back(MountArg(mount));
    }
    return sandbox_common::DockerRunArgs(spec);
  };

  // Background steps first, detached, so the foreground one has something to
  // talk to.
  for (const proto::Step &step : phase.background()) {
    if (observer != nullptr) {
      observer->OnStepStarted(job.id(), phase.name(), step.name());
    }
    const process::Options opts = {
        .stdout_path = log_dir / (step.name() + "_start.out"),
        .stderr_path = log_dir / (step.name() + "_start.err")};
    const process::RunResult started = process::RunCommand(
        config_.docker, container_args(step, /*detached=*/true), opts,
        std::chrono::seconds(120));
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

  // Whatever the steps were asked to bring home, copied out of the exited
  // container's scratch through the daemon. The container was kept for this.
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
      const std::filesystem::path local =
          log_dir / "collected" / step->name() / file;
      std::error_code ec;
      std::filesystem::create_directories(local.parent_path(), ec);
      std::filesystem::remove(local, ec);
      sandbox_common::RunStep(
          config_.docker,
          {"cp",
           SandboxName(job.id(), step->name()) + ":" +
               std::string(sandbox_common::kScratch) + "/" + file,
           local.string()},
          /*cwd=*/{}, log_dir, "collect_" + step->name(),
          std::chrono::seconds(120));
      const std::string content = ReadFile(local);
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
