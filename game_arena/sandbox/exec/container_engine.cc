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
using sandbox_common::StepResult;
using sandbox_common::TailOf;

constexpr int kDefaultDrainTimeoutS = 60;

StepResult Docker(const std::string &docker,
                  const std::vector<std::string> &args,
                  const std::filesystem::path &log_dir, const std::string &tag,
                  int timeout_s = 60) {
  return sandbox_common::RunStep(docker, args, /*cwd=*/{}, log_dir, tag,
                                 std::chrono::seconds(timeout_s));
}

// Cleanup is best effort; its complaints would only clutter the worker's log.
void Quiet(const std::string &docker, const std::vector<std::string> &args) {
  process::RunCommand(docker, args,
                      {.stdout_path = "/dev/null", .stderr_path = "/dev/null"},
                      std::chrono::seconds(60));
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

}  // namespace

ContainerEngine::ContainerEngine(ContainerEngineConfig config)
    : config_(std::move(config)) {}

proto::JobResult ContainerEngine::Run(const proto::Job &job,
                                      Observer *observer) {
  proto::JobResult result;
  proto::Status *status = result.mutable_status();

  if (job.phases().empty()) {
    Fail(status, proto::Status::INVALID_JOB, "a job needs at least one phase");
    return result;
  }

  if (job.isolation().image().empty()) {
    Fail(status, proto::Status::INVALID_JOB,
         "a container job needs an image to load its workspace with");
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
    const StepResult made = Docker(config_.docker, {"volume", "create", volume},
                                   log_dir, "volume_" + volume);
    if (!made.run.started) {
      return Fail(status, proto::Status::TOOL_MISSING,
                  "cannot run docker ('" + config_.docker + "' not found)");
    }
    if (made.run.exit_code != 0) {
      return Fail(
          status, proto::Status::WORKSPACE_FAILED,
          "cannot create volume " + volume + ": " + TailOf(made.output, 500));
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
  Quiet(config_.docker, {"rm", "-f", loader});
  const auto fail_load = [&](const std::string &what, const StepResult &step) {
    Quiet(config_.docker, {"rm", "-f", loader});
    return Fail(status, proto::Status::WORKSPACE_FAILED,
                what + ": " + TailOf(step.output, 1000));
  };

  const StepResult created =
      Docker(config_.docker, sandbox_common::DockerRunArgs(spec), log_dir,
             "load_create", 120);
  if (!created.run.started || created.run.exit_code != 0) {
    return fail_load("cannot create the workspace loader", created);
  }

  if (!ws.staging_dir().empty() && !ws.staged_files().empty()) {
    const StepResult copied =
        Docker(config_.docker,
               {"cp", ws.staging_dir() + "/.",
                loader + ":" + sandbox_common::kPatchMount},
               log_dir, "load_patches", 120);
    if (!copied.run.started || copied.run.exit_code != 0) {
      return fail_load("cannot load the staged files into the sandbox", copied);
    }
  }

  const StepResult ran = Docker(config_.docker, {"start", "-a", loader},
                                log_dir, "load_start", 600);
  if (!ran.run.started || ran.run.exit_code != 0) {
    return fail_load("the workspace loader failed", ran);
  }
  Quiet(config_.docker, {"rm", "-f", loader});
  return true;
}

void ContainerEngine::RemoveVolumes(const proto::Job &job) {
  for (const std::string &volume : JobVolumes(job)) {
    Quiet(config_.docker, {"volume", "rm", "-f", volume});
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
      Quiet(config_.docker, {"rm", "-f", SandboxName(job.id(), step->name())});
    }
    if (!network.empty()) {
      Quiet(config_.docker, {"network", "rm", network});
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
    const StepResult made =
        Docker(config_.docker, {"network", "create", "--internal", network},
               log_dir, phase.name() + "_network");
    if (!made.run.started) {
      return Fail(status, proto::Status::TOOL_MISSING,
                  "cannot run docker ('" + config_.docker + "' not found)");
    }
    if (made.run.exit_code != 0) {
      teardown();
      return Fail(
          status, proto::Status::NETWORK_FAILED,
          "cannot create the phase network: " +
              TailOf(ReadFile(log_dir / (phase.name() + "_network.err")), 500));
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
    const StepResult started =
        Docker(config_.docker, container_args(step, /*detached=*/true), log_dir,
               step.name() + "_start", 120);
    if (!started.run.started || started.run.exit_code != 0) {
      teardown();
      return Fail(
          status, proto::Status::START_FAILED,
          "cannot start " + step.name() + ": " +
              TailOf(ReadFile(log_dir / (step.name() + "_start.err")), 1000));
    }
  }

  const proto::Step &foreground = phase.foreground();
  const StepResult ran =
      Docker(config_.docker, container_args(foreground, /*detached=*/false),
             log_dir, foreground.name(), foreground.timeout_s());

  proto::StepResult *foreground_result =
      AddStepResult(result, foreground.name(), log_dir);
  foreground_result->set_started(ran.run.started);
  foreground_result->set_timeout_s(foreground.timeout_s());

  if (!ran.run.started) {
    teardown();
    return Fail(status, proto::Status::TOOL_MISSING,
                "cannot run docker ('" + config_.docker + "' not found)");
  }
  if (ran.run.timed_out) {
    // The timeout killed the docker *client*; the container belongs to the
    // daemon and has to be stopped by name.
    Quiet(config_.docker, {"kill", SandboxName(job.id(), foreground.name())});
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
    Docker(config_.docker, {"wait", container}, log_dir, step.name() + "_wait",
           drain_timeout_s);
    Docker(config_.docker, {"logs", container}, log_dir, step.name());
    AddStepResult(result, step.name(), log_dir);
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
      Docker(config_.docker,
             {"cp",
              SandboxName(job.id(), step->name()) + ":" +
                  std::string(sandbox_common::kScratch) + "/" + file,
              local.string()},
             log_dir, "collect_" + step->name(), 120);
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
    Quiet(config_.docker, {"kill", container});
  }
  // The network too. Before this engine, a cancelled match left one behind
  // per order: teardown only ran on Run's own exit paths.
  for (const std::string &network : networks) {
    Quiet(config_.docker, {"network", "rm", network});
  }
}

}  // namespace sandbox_exec
