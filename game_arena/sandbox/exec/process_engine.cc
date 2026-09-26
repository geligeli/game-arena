#include "game_arena/sandbox/exec/process_engine.h"

#include <signal.h>
#include <unistd.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "game_arena/common/process/process.h"
#include "game_arena/sandbox/common/files.h"
#include "game_arena/sandbox/common/text.h"
#include "game_arena/sandbox/exec/workspace.h"

namespace sandbox_exec {

namespace {

using sandbox_common::ReadFile;
using sandbox_common::TailOf;

// A step's isolation, or the phase's, or the job's. Same precedence the
// container engine uses: a step's isolation replaces rather than merges, so
// half-overridden isolation cannot read as tight and not be.
proto::Isolation EffectiveIsolation(const proto::Job &job,
                                    const proto::Phase &phase,
                                    const proto::Step &step) {
  if (step.isolation().ByteSizeLong() > 0) {
    return step.isolation();
  }
  if (phase.isolation().ByteSizeLong() > 0) {
    return phase.isolation();
  }
  return job.isolation();
}

void Fail(proto::Status *status, proto::Status::Code code,
          const std::string &message, const std::string &phase,
          const std::string &step) {
  status->set_code(code);
  status->set_message(message);
  status->set_phase(phase);
  status->set_step(step);
}

// Polls for a step's port file. Polling rather than a pipe because a
// background step is started detached and its stdout is a log, not a channel.
int AwaitPort(const std::filesystem::path &port_file,
              std::chrono::seconds limit) {
  const auto deadline = std::chrono::steady_clock::now() + limit;
  while (std::chrono::steady_clock::now() < deadline) {
    std::ifstream in(port_file);
    int port = 0;
    if (in && (in >> port) && port > 0) {
      return port;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  return 0;
}

}  // namespace

ProcessEngine::ProcessEngine(ProcessEngineConfig config)
    : config_(std::move(config)) {}

void ProcessEngine::Track(const std::string &job_id, pid_t pgid) {
  std::lock_guard<std::mutex> lock(mutex_);
  running_.emplace(job_id, pgid);
}

void ProcessEngine::Untrack(const std::string &job_id, pid_t pgid) {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto range = running_.equal_range(job_id);
  for (auto it = range.first; it != range.second; ++it) {
    if (it->second == pgid) {
      running_.erase(it);
      return;
    }
  }
}

proto::JobResult ProcessEngine::Run(const proto::Job &job, Observer *observer) {
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
    cancelled_.erase(job.id());
  }

  for (const proto::Phase &phase : job.phases()) {
    proto::PhaseResult *phase_result = result.add_phases();
    phase_result->set_name(phase.name());
    if (!RunPhase(job, phase, observer, phase_result, status)) {
      break;
    }
  }

  {
    std::lock_guard<std::mutex> lock(mutex_);
    // A phase that failed early stopped its background steps untracked.
    running_.erase(job.id());
    if (cancelled_.erase(job.id()) > 0) {
      // Unconditionally, including over an OK status: a killed step merely
      // exits nonzero, which on its own is indistinguishable from a step that
      // failed on its own merits.
      status->set_code(proto::Status::CANCELLED);
      status->set_message("cancelled");
    }
  }
  return result;
}

bool ProcessEngine::RunPhase(const proto::Job &job, const proto::Phase &phase,
                             Observer *observer, proto::PhaseResult *result,
                             proto::Status *status) {
  const std::filesystem::path log_dir(job.log_dir());
  const std::filesystem::path tree(job.workspace().tree_dir());
  const auto scratch = [&](const proto::Step &step) {
    const std::filesystem::path shared(job.workspace().scratch_dir());
    if (!step.private_scratch()) {
      return shared;
    }
    std::error_code ec;
    std::filesystem::create_directories(shared / step.name(), ec);
    return shared / step.name();
  };

  // Resolved addresses of the background steps, for {{peer:<name>}}.
  std::map<std::string, std::string> peers;
  std::vector<process::Child> background;

  // Every placeholder this engine can resolve, rebuilt per step because
  // {{port_file}} is per step and the peer addresses are only known once the
  // background steps have published them.
  const auto resolve = [&](const proto::Step &step) -> proto::Step {
    std::map<std::string, std::string> replacements = {
        {kScratchPlaceholder, scratch(step).string()},
        {kPortFilePlaceholder, (log_dir / (step.name() + ".port")).string()}};
    for (const auto &[name, address] : peers) {
      replacements["{{peer:" + name + "}}"] = address;
    }
    return Substituted(step, replacements);
  };

  const auto render = [&](const proto::Step &step) -> std::vector<std::string> {
    const proto::Step resolved = resolve(step);
    std::vector<std::string> argv;
    for (const proto::Token &token : resolved.argv()) {
      argv.push_back(token.text());
    }
    return argv;
  };

  if (observer != nullptr) {
    observer->OnPhaseStarted(job.id(), phase.name());
  }

  for (const proto::Step &step : phase.background()) {
    if (observer != nullptr) {
      observer->OnStepStarted(job.id(), phase.name(), step.name());
    }
    const std::vector<std::string> argv = render(step);
    if (argv.empty()) {
      Fail(status, proto::Status::INVALID_JOB, "a step needs a command",
           phase.name(), step.name());
      return false;
    }
    const std::filesystem::path port_file = log_dir / (step.name() + ".port");
    std::error_code ec;
    std::filesystem::remove(port_file, ec);
    std::optional<process::Child> child = process::Child::Start(
        argv.front(), {argv.begin() + 1, argv.end()},
        {.stdout_path = log_dir / (step.name() + ".out"),
         .stderr_path = log_dir / (step.name() + ".err")});
    if (!child) {
      Fail(status, proto::Status::START_FAILED,
           step.name() + " is missing at " + argv.front(), phase.name(),
           step.name());
      return false;
    }
    Track(job.id(), child->pid());
    background.push_back(std::move(*child));

    if (step.endpoint().discover_via_port_file()) {
      // The port is only knowable once the step is listening. Waiting for the
      // file it writes after bind() is the difference between "the peer could
      // not connect" and "it connected before anything was there".
      const int timeout_s = step.endpoint().discover_timeout_s() > 0
                                ? step.endpoint().discover_timeout_s()
                                : config_.endpoint_timeout_s;
      const int port = AwaitPort(port_file, std::chrono::seconds(timeout_s));
      if (port <= 0) {
        Fail(status, proto::Status::ENDPOINT_FAILED,
             step.name() + " never reported a port: " +
                 TailOf(ReadFile(log_dir / (step.name() + ".err")), 1500),
             phase.name(), step.name());
        return false;
      }
      peers[step.name()] = "localhost:" + std::to_string(port);
    } else if (step.endpoint().port() > 0) {
      peers[step.name()] =
          "localhost:" + std::to_string(step.endpoint().port());
    }
  }

  const proto::Step &foreground = phase.foreground();
  if (observer != nullptr) {
    observer->OnStepStarted(job.id(), phase.name(), foreground.name());
  }
  const std::vector<std::string> argv = render(foreground);
  if (argv.empty()) {
    Fail(status, proto::Status::INVALID_JOB, "a step needs a command",
         phase.name(), foreground.name());
    return false;
  }

  const proto::Step resolved_foreground = resolve(foreground);
  std::vector<std::string> foreground_env;
  for (const auto &[key, value] : resolved_foreground.env()) {
    foreground_env.push_back(key + "=" + value);
  }

  std::optional<process::Child> child = process::Child::Start(
      argv.front(), {argv.begin() + 1, argv.end()},
      {.cwd = foreground.cwd().empty()
                  ? tree
                  : std::filesystem::path(foreground.cwd()),
       .env = foreground_env,
       .stdout_path = log_dir / (foreground.name() + ".out"),
       .stderr_path = log_dir / (foreground.name() + ".err"),
       .address_space_limit_bytes = EffectiveIsolation(job, phase, foreground)
                                        .address_space_limit_bytes()});
  bool timed_out = false;
  int exit_code = -1;
  if (child) {
    Track(job.id(), child->pid());
    timed_out = !child->Wait(std::chrono::seconds(foreground.timeout_s()));
    exit_code = child->Stop(std::chrono::seconds(5));
    Untrack(job.id(), child->pid());
  }

  proto::StepResult *foreground_result = result->add_steps();
  foreground_result->set_name(foreground.name());
  foreground_result->set_started(child.has_value());
  foreground_result->set_timeout_s(foreground.timeout_s());
  foreground_result->set_timed_out(timed_out);
  foreground_result->set_exit_code(timed_out ? 124 : exit_code);
  foreground_result->set_stdout(
      ReadFile(log_dir / (foreground.name() + ".out")));
  foreground_result->set_stderr(
      ReadFile(log_dir / (foreground.name() + ".err")));

  if (!child) {
    Fail(status, proto::Status::START_FAILED, "cannot run " + argv.front(),
         phase.name(), foreground.name());
    return false;
  }

  // The background steps are done being talked to; let them finish writing
  // their own verdicts.
  for (process::Child &step : background) {
    step.Wait();
    Untrack(job.id(), step.pid());
  }
  for (const proto::Step &step : phase.background()) {
    proto::StepResult *background_result = result->add_steps();
    background_result->set_name(step.name());
    background_result->set_started(true);
    background_result->set_stdout(ReadFile(log_dir / (step.name() + ".out")));
    background_result->set_stderr(ReadFile(log_dir / (step.name() + ".err")));
    if (!peers[step.name()].empty()) {
      background_result->set_peer_address(peers[step.name()]);
    }
  }

  std::vector<const proto::Step *> all;
  for (const proto::Step &step : phase.background()) {
    all.push_back(&step);
  }
  all.push_back(&foreground);
  for (proto::StepResult &step_result : *result->mutable_steps()) {
    for (const proto::Step *step : all) {
      if (step->name() != step_result.name()) {
        continue;
      }
      for (const std::string &file : step->collect_files()) {
        const std::string content = ReadFile(scratch(*step) / file);
        if (!content.empty()) {
          (*step_result.mutable_collected())[file] = content;
        }
      }
    }
  }

  return foreground_result->exit_code() == 0 && !foreground_result->timed_out();
}

void ProcessEngine::Cancel(const std::string &job_id) {
  std::vector<pid_t> groups;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto range = running_.equal_range(job_id);
    if (range.first == range.second) {
      return;
    }
    cancelled_.insert(job_id);
    for (auto it = range.first; it != range.second; ++it) {
      groups.push_back(it->second);
    }
  }
  // The group, not the process: build tools spawn trees, and killing only the
  // parent leaves the workers building. Signalling one that has already
  // exited is a no-op, which is the race worth designing for.
  for (const pid_t pgid : groups) {
    ::killpg(pgid, SIGKILL);
  }
}

}  // namespace sandbox_exec
