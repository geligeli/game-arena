#include "game_arena/sandbox/worker/docker_backend.h"

#include <system_error>
#include <utility>

#include "absl/log/log.h"
#include "game_arena/common/kv_options/kv_options.h"
#include "game_arena/common/metric_report/metric_report.h"
#include "game_arena/common/process/process.h"
#include "game_arena/sandbox/common/files.h"
#include "game_arena/sandbox/common/text.h"
#include "game_arena/sandbox/worker/bot_launch.h"
#include "game_arena/sandbox/worker/build_log.h"
#include "game_arena/sandbox/worker/checkout.h"
#include "game_arena/sandbox/worker/grade_policy.h"
#include "game_arena/sandbox/worker/match_tally.h"

namespace tournament_arena {

namespace {

using sandbox_common::BindMount;
using sandbox_common::HostOverlayPrelude;
using sandbox_common::OverlayMountScript;
using sandbox_common::ReadFile;
using sandbox_common::ShellQuote;
using sandbox_common::TailOf;

// Both sides of an order, primary first. A builtin opponent contributes none.
auto SidesOf(const proto::WorkOrder &order)
    -> std::vector<const proto::Side *> {
  std::vector<const proto::Side *> sides = {&order.candidate()};
  if (order.has_opponent()) {
    sides.push_back(&order.opponent());
  }
  return sides;
}

}  // namespace

auto DockerBuildScript(const std::filesystem::path &disk_cache,
                       const std::vector<std::string> &bazel_flags,
                       const std::vector<std::string> &patch_files,
                       const std::vector<std::string> &targets,
                       bool mount_in_container) -> std::string {
  // set -eu: a patch that does not apply aborts here, with git's own message in
  // the build log, rather than becoming a confusing compile error later.
  std::string script = "set -eu\n";
  script += mount_in_container ? OverlayMountScript() : HostOverlayPrelude();
  script += "cd " + std::string(sandbox_common::kWorkspace) + "\n";
  for (const std::string &patch : patch_files) {
    script +=
        "git apply " +
        ShellQuote(std::string(sandbox_common::kPatchMount) + "/" + patch) +
        "\n";
  }
  script += std::string("exec bazel --output_base=") +
            sandbox_common::kOutputBaseMount;
  if (!disk_cache.empty()) {
    script += std::string(" --disk_cache=") + sandbox_common::kDiskCacheMount;
  }
  for (const std::string &flag : bazel_flags) {
    script += " " + ShellQuote(flag);
  }
  script += " build";
  for (const std::string &target : targets) {
    script += " " + ShellQuote(target);
  }
  script += "\n";
  return script;
}

auto DockerGradeScript(const std::string &report_path,
                       const std::vector<std::string> &argv,
                       bool mount_in_container) -> std::string {
  std::string script = "set -eu\n";
  script += mount_in_container ? OverlayMountScript() : HostOverlayPrelude();
  script += "cd " + std::string(sandbox_common::kWorkspace) + "\n";
  // Where the command writes its numbers. Exported rather than fixed, so the
  // command needs no knowledge of the sandbox's directory layout.
  script += "export ARENA_REPORT=" + ShellQuote(report_path) + "\n";
  script += "exec";
  for (const std::string &arg : argv) {
    script += " " + ShellQuote(arg);
  }
  script += "\n";
  return script;
}

auto DockerRunScript(const std::string &bot_path,
                     const std::vector<std::string> &args,
                     bool mount_in_container) -> std::string {
  std::string script = "set -eu\n";
  script += mount_in_container ? OverlayMountScript() : HostOverlayPrelude();
  script += "cd " + std::string(sandbox_common::kWorkspace) + "\n";
  script += "exec " + ShellQuote(bot_path);
  for (const std::string &arg : args) {
    script += " " + ShellQuote(arg);
  }
  script += "\n";
  return script;
}

DockerBackend::DockerBackend(DockerBackendConfig config)
    : config_(std::move(config)) {}

auto DockerBackend::SlotDir(int slot) const -> std::filesystem::path {
  return config_.work_dir / ("slot" + std::to_string(slot));
}

auto DockerBackend::RepoDir(int slot) const -> std::filesystem::path {
  return SlotDir(slot) / "repo";
}

auto DockerBackend::OverlayDir(int slot) const -> std::filesystem::path {
  return SlotDir(slot) / "overlay";
}

auto DockerBackend::OutputBase(int slot) const -> std::filesystem::path {
  return SlotDir(slot) / "bazel_output_base";
}

auto DockerBackend::PatchDir(int slot) const -> std::filesystem::path {
  return SlotDir(slot) / "patches";
}

auto DockerBackend::Warmup(int slots, std::string *error) -> bool {
  if (!std::filesystem::is_directory(config_.repo_dir)) {
    *error = "--repo '" + config_.repo_dir.string() +
             "' is not a directory; the docker backend needs a local path, "
             "because it is mounted into the containers";
    return false;
  }
  if (!std::filesystem::exists(config_.repo_dir / ".git")) {
    *error = "--repo '" + config_.repo_dir.string() +
             "' is not a git repository; the backend checks candidates out at "
             "a commit";
    return false;
  }
  std::error_code ec;
  // Bind-mount sources must exist before docker will accept them.
  std::filesystem::create_directories(config_.disk_cache, ec);
  for (int slot = 0; slot < slots; ++slot) {
    std::filesystem::create_directories(OverlayDir(slot), ec);
    std::filesystem::create_directories(OutputBase(slot), ec);
    if (ec) {
      *error = "cannot create slot directories under " +
               config_.work_dir.string() + ": " + ec.message();
      return false;
    }
    if (!EnsureClone(config_.git, config_.repo_dir, RepoDir(slot),
                     SlotDir(slot) / "logs", error)) {
      return false;
    }
  }
  return true;
}

auto DockerBackend::PrepareCheckout(int slot, const std::string &base_commit,
                                    std::string *error) -> bool {
  const std::filesystem::path logs = SlotDir(slot) / "logs";
  return EnsureClone(config_.git, config_.repo_dir, RepoDir(slot), logs,
                     error) &&
         SyncToCommit(config_.git, RepoDir(slot), base_commit, logs, error);
}

auto DockerBackend::StageCandidate(int slot, const proto::WorkOrder &order,
                                   std::string *error) -> bool {
  // The previous order's patch lives in the slot's overlay upper, not in the
  // clone. Clear the whole upper: a patch may touch anything the problem
  // allows, so anything may need undoing, and the merged tree has to be exactly
  // the clone plus this order.
  //
  // This is not what makes a slot warm -- the persistent bazel output base is,
  // and that is a separate mount that survives.
  std::error_code ec;
  std::filesystem::remove_all(OverlayDir(slot) / "upper", ec);
  std::filesystem::remove_all(OverlayDir(slot) / "work", ec);

  std::filesystem::remove_all(PatchDir(slot), ec);
  std::filesystem::create_directories(PatchDir(slot), ec);
  if (ec) {
    *error = "cannot create patch directory " + PatchDir(slot).string() + ": " +
             ec.message();
    return false;
  }

  for (const proto::Side *side : SidesOf(order)) {
    if (side->patch().empty()) {
      *error = "submission " + side->candidate_id() + " carries no patch";
      return false;
    }
    const std::filesystem::path path =
        PatchDir(slot) /
        (sandbox_common::SanitizeContainerName(side->candidate_id()) + ".diff");
    if (!sandbox_common::WriteFile(path, side->patch(), error)) {
      *error =
          "cannot stage the patch for " + side->candidate_id() + ": " + *error;
      return false;
    }
  }
  return true;
}

auto DockerBackend::RunOrder(int slot,
                             const proto::WorkOrder &order) -> OrderOutcome {
  OrderOutcome outcome;
  const std::filesystem::path logs = SlotDir(slot) / "logs";

  std::string error;
  if (!PrepareCheckout(slot, order.base_commit(), &error) ||
      !StageCandidate(slot, order, &error)) {
    outcome.error = error;
    return outcome;
  }

  // Assembled on the host, so no container needs CAP_SYS_ADMIN. There is no
  // fallback to the in-container mount: quietly running submitted build code
  // privileged because a mount failed is the downgrade nobody notices.
  if (!MountOverlay(slot, &error)) {
    outcome.error = error;
    return outcome;
  }
  struct OverlayGuard {
    DockerBackend *backend;
    int slot;
    ~OverlayGuard() { backend->UnmountOverlay(slot); }
  } overlay_guard{this, slot};

  const std::string name_base = ContainerNameBase(slot, order.order_id());
  {
    std::lock_guard lock(mutex_);
    running_slots_[order.order_id()] = slot;
  }
  // Cleared on every exit path, so a Cancel for a finished order derives
  // nothing and does nothing.
  struct SlotGuard {
    DockerBackend *backend;
    std::string order_id;
    ~SlotGuard() {
      std::lock_guard lock(backend->mutex_);
      backend->running_slots_.erase(order_id);
    }
  } slot_guard{this, order.order_id()};

  std::vector<std::string> build_mounts = WorkspaceMounts(slot);
  build_mounts.push_back(
      BindMount(PatchDir(slot), sandbox_common::kPatchMount, true));
  if (!config_.disk_cache.empty()) {
    build_mounts.push_back(
        BindMount(config_.disk_cache, sandbox_common::kDiskCacheMount, false));
  }
  // Everything this order needs, in one build: both sides and the referee that
  // will judge them. They share an output base, so building them together is
  // cheaper than three passes and guarantees one consistent tree.
  std::vector<std::string> targets;
  std::vector<std::string> patch_files;
  for (const proto::Side *side : SidesOf(order)) {
    patch_files.push_back(
        sandbox_common::SanitizeContainerName(side->candidate_id()) + ".diff");
    for (const std::string &target : side->build_targets()) {
      targets.push_back(target);
    }
  }
  if (!order.referee_target().empty()) {
    targets.push_back(order.referee_target());
  }
  const std::string build_script =
      DockerBuildScript(config_.disk_cache, config_.bazel_flags, patch_files,
                        targets, !config_.host_overlay);
  const std::string build_name = name_base + "-build";
  {
    const std::chrono::seconds build_timeout = std::chrono::seconds(
        order.build_timeout_s() > 0 ? order.build_timeout_s() : 1800);
    process::RunOptions options;
    options.stdout_path = logs / (build_name + ".out");
    options.stderr_path = logs / (build_name + ".err");
    options.timeout = build_timeout;

    // A worker that was killed mid-order leaves its container running under
    // this exact name; clear it so a redelivered order can start fresh.
    sandbox_common::RemoveContainer(config_.docker, build_name);

    // A build that can fetch can also exfiltrate, and a submitted genrule is
    // arbitrary code. The image is expected to carry a warm repository cache.
    const std::vector<std::string> args = sandbox_common::DockerRunArgs({
        /*name=*/build_name,
        /*image=*/config_.docker_image,
        /*script=*/build_script,
        /*rm=*/true,
        /*detached=*/false,
        /*network=*/config_.allow_build_network ? "bridge" : "none",
        /*extra_args=*/HardeningArgs(),
        /*mounts=*/build_mounts,
    });

    const process::RunResult build =
        process::RunCommand(config_.docker, args, options);
    const std::string build_output =
        ReadFile(options.stdout_path) + ReadFile(options.stderr_path);
    if (!build.started) {
      outcome.error = "cannot run docker ('" + config_.docker + "' not found)";
      return outcome;
    }
    if (build.timed_out) {
      // The timeout killed the docker *client*; the container is managed by
      // the daemon and would otherwise keep building.
      sandbox_common::KillContainer(config_.docker, build_name);
      outcome.build_log = CompactBuildLog(build_output);
      outcome.error = "build timed out after " +
                      std::to_string(build_timeout.count()) + "s";
      return outcome;
    }
    if (build.exit_code != 0) {
      // A build failure is the candidate's fault, not the order's: report it
      // as a completed order with build_ok=false so the agent gets the
      // diagnostics.
      outcome.build_log = CompactBuildLog(build_output);
      return outcome;
    }
    outcome.build_ok = true;
  }

  if (order.has_grade()) {
    return RunGrade(slot, order, std::move(outcome), name_base, logs);
  }

  // The match runs on a private, egress-free bridge: the two bots reach their
  // referee by container name through docker's embedded DNS, and nothing else.
  // That is only possible because the referee is here rather than on the
  // coordinator -- there is no longer a broker on the outside to dial.
  const std::string network = name_base + "-net";
  const std::string referee_name = name_base + "-referee";
  const std::string bot_name = name_base + "-bot";
  const std::string opponent_name = name_base + "-opponent";
  const int run_timeout_s =
      order.run_timeout_s() > 0 ? order.run_timeout_s() : 1800;

  // Everything this match creates, removed on every exit path below.
  const auto cleanup = [&] {
    for (const std::string &name : {referee_name, bot_name, opponent_name}) {
      sandbox_common::RemoveContainer(config_.docker, name);
    }
    sandbox_common::RemoveNetwork(config_.docker, network);
  };
  // A worker killed mid-order leaves containers behind under these exact
  // names; clear them so a redelivered order can start fresh.
  cleanup();

  {
    const sandbox_common::StepResult made =
        sandbox_common::CreateInternalNetwork(config_.docker, network, logs,
                                              "network");
    if (!made.run.started || made.run.exit_code != 0) {
      outcome.error = "cannot create the match network: " +
                      TailOf(ReadFile(logs / "network.err"), 500);
      return outcome;
    }
  }

  const std::vector<std::string> match_mounts = WorkspaceMounts(slot);

  // Assembles a `docker run` for one member of the match. A detached container
  // returns immediately; the others block until their process exits.
  const auto container_args =
      [&](const std::string &name, bool detached,
          const std::string &script) -> std::vector<std::string> {
    return sandbox_common::DockerRunArgs({
        /*name=*/name,
        /*image=*/config_.docker_image,
        /*script=*/script,
        /*rm=*/false,  // the referee's verdict is read back via `docker logs`
        /*detached=*/detached,
        /*network=*/network,
        /*extra_args=*/HardeningArgs(),
        /*mounts=*/match_mounts,
    });
  };

  const int match_deadline_s = order.match_deadline_s() > 0
                                   ? order.match_deadline_s()
                                   : std::max(1, run_timeout_s - 30);
  std::vector<std::string> referee_args = {
      "--port=" + std::to_string(kMatchPort),
      "--game=" + order.game(),
      "--games=" + std::to_string(order.num_games()),
      "--player_a=" + order.candidate().candidate_id(),
      "--player_b=" + order.opponent_spec(),
      "--deadline_s=" + std::to_string(match_deadline_s),
  };
  // Opaque to the worker: whatever the problem set, handed to the registry
  // linked into the referee. Omitted entirely when empty so an order that
  // sets nothing produces the argv it always did.
  if (!order.registry_options().empty()) {
    referee_args.push_back(
        "--registry_options=" +
        kv_options::Format({order.registry_options().begin(),
                            order.registry_options().end()}));
  }
  {
    process::RunOptions opts;
    opts.timeout = std::chrono::seconds(120);
    opts.stdout_path = logs / "referee_start.out";
    opts.stderr_path = logs / "referee_start.err";
    const process::RunResult started = process::RunCommand(
        config_.docker,
        container_args(
            referee_name, /*detached=*/true,
            DockerRunScript(
                "./bazel-bin/" + BinaryPathForTarget(order.referee_target()),
                referee_args, !config_.host_overlay)),
        opts);
    if (!started.started || started.exit_code != 0) {
      outcome.error = "cannot start the referee: " +
                      TailOf(ReadFile(opts.stderr_path), 1000);
      cleanup();
      return outcome;
    }
  }

  // The referee resolves by container name on this network, so no port has to
  // be discovered or forwarded.
  const std::string target = referee_name + ":" + std::to_string(kMatchPort);
  const std::string bot_path =
      "./bazel-bin/" + BinaryPathForTarget(order.candidate().bot_target());

  if (order.has_opponent()) {
    const std::string opponent_path =
        "./bazel-bin/" + BinaryPathForTarget(order.opponent().bot_target());
    process::RunOptions opts;
    opts.timeout = std::chrono::seconds(120);
    opts.stdout_path = logs / "opponent_start.out";
    opts.stderr_path = logs / "opponent_start.err";
    process::RunCommand(
        config_.docker,
        container_args(
            opponent_name, /*detached=*/true,
            DockerRunScript(opponent_path,
                            BotArgs(order.opponent().candidate_id(), target,
                                    std::string(kPlayerPrefix) +
                                        order.candidate().candidate_id(),
                                    order.num_games(),
                                    FormatParams(order.opponent().params())),
                            !config_.host_overlay)),
        opts);
  }

  {
    process::RunOptions opts;
    opts.timeout = std::chrono::seconds(run_timeout_s);
    opts.stdout_path = logs / (bot_name + ".out");
    opts.stderr_path = logs / (bot_name + ".err");
    const process::RunResult run = process::RunCommand(
        config_.docker,
        container_args(
            bot_name, /*detached=*/false,
            DockerRunScript(bot_path,
                            BotArgs(order.candidate().candidate_id(), target,
                                    order.opponent_spec(), order.num_games(),
                                    FormatParams(order.candidate().params())),
                            !config_.host_overlay)),
        opts);
    if (!run.started) {
      outcome.error = "cannot run docker ('" + config_.docker + "' not found)";
      cleanup();
      return outcome;
    }
    if (run.timed_out) {
      // The timeout killed the docker *client*; the container is the daemon's
      // and would otherwise keep playing.
      sandbox_common::KillContainer(config_.docker, bot_name);
      outcome.error =
          "games timed out after " + std::to_string(run_timeout_s) + "s";
      cleanup();
      return outcome;
    }
  }

  // Wait for the referee to finish counting, then read its verdict. Its own
  // --deadline_s bounds this, so a bot that exited early cannot hang the slot.
  sandbox_common::WaitForContainer(config_.docker, referee_name,
                                   std::chrono::seconds(match_deadline_s + 60),
                                   logs, "referee_wait");

  sandbox_common::ContainerLogs(config_.docker, referee_name, logs, "referee");
  const std::string referee_output = ReadFile(logs / "referee.out");
  const std::string referee_errors = ReadFile(logs / "referee.err");
  cleanup();

  // The referee's tally, not the bot's: a bot only knows what it was told,
  // while the referee applied every move and decided every game.
  RunTally tally;
  if (!ParseResultLine(referee_output, &tally)) {
    outcome.error = "referee produced no result: " +
                    TailOf(referee_errors + referee_output, 1500);
    return outcome;
  }
  if (tally.games < order.num_games()) {
    // Recorded, not fatal: the games that were played are real results, and an
    // agent is better served by a short match plus the reason than by nothing.
    outcome.error = "match was short: " + std::to_string(tally.games) + " of " +
                    std::to_string(order.num_games()) + " games played";
  }
  outcome.games_played = tally.games;
  outcome.wins = tally.wins;
  outcome.draws = tally.draws;
  outcome.losses = tally.losses;
  outcome.elo = tally.elo;
  return outcome;
}

auto DockerBackend::RunGrade(int slot, const proto::WorkOrder &order,
                             OrderOutcome outcome, const std::string &name_base,
                             const std::filesystem::path &logs)
    -> OrderOutcome {
  const proto::GradeOrder &grade = order.grade();
  if (grade.argv().empty()) {
    outcome.error = "graded order carries no command to run";
    return outcome;
  }
  const int repeats = std::max(1, grade.repeats());
  const int timeout_s = grade.timeout_s() > 0 ? grade.timeout_s() : 1800;

  const std::vector<std::string> mounts = WorkspaceMounts(slot);

  std::vector<std::map<std::string, double>> runs;
  for (int run = 0; run < repeats; ++run) {
    // One name for every run: they are sequential, and a stable name is what
    // lets Cancel find the container without knowing how far along the repeats
    // have got.
    const std::string name = name_base + "-grade";
    // The report is written inside the container's scratch mount, which is the
    // slot's overlay upper on the host -- so the worker can read it back
    // without the container writing anywhere else.
    const std::string in_container_report =
        std::string(sandbox_common::kScratch) + "/report.json";
    const std::filesystem::path host_report = OverlayDir(slot) / "report.json";
    std::error_code ec;
    std::filesystem::remove(host_report, ec);

    sandbox_common::RemoveContainer(config_.docker, name);

    process::RunOptions options;
    options.stdout_path = logs / (name + std::to_string(run) + ".out");
    options.stderr_path = logs / (name + std::to_string(run) + ".err");
    options.timeout = std::chrono::seconds(timeout_s);

    const std::vector<std::string> args = sandbox_common::DockerRunArgs({
        /*name=*/name,
        /*image=*/config_.docker_image,
        /*script=*/
        DockerGradeScript(in_container_report,
                          {grade.argv().begin(), grade.argv().end()},
                          !config_.host_overlay),
        /*rm=*/true,
        /*detached=*/false,
        // A timed run has no business reaching the network.
        /*network=*/"none",
        /*extra_args=*/HardeningArgs(),
        /*mounts=*/mounts,
    });

    const process::RunResult result =
        process::RunCommand(config_.docker, args, options);
    const std::string output = ReadFile(options.stdout_path);
    if (!result.started) {
      outcome.error = "cannot run docker ('" + config_.docker + "' not found)";
      return outcome;
    }
    if (result.timed_out) {
      sandbox_common::KillContainer(config_.docker, name);
      outcome.error =
          "graded run timed out after " + std::to_string(timeout_s) + "s";
      return outcome;
    }
    if (result.exit_code != 0) {
      // A nonzero exit means the measurement is not trustworthy, whatever it
      // printed. Reporting it anyway would put a number on a failed run.
      outcome.error = "graded run exited " + std::to_string(result.exit_code) +
                      ": " +
                      TailOf(ReadFile(options.stderr_path) + output, 1500);
      return outcome;
    }

    std::map<std::string, double> metrics;
    if (!metric_report::Parse(ReadFile(host_report), output, &metrics)) {
      outcome.error =
          "graded run produced no metrics: write JSON to $ARENA_REPORT or "
          "print a RESULT line. Output was: " +
          TailOf(output, 1000);
      return outcome;
    }
    runs.push_back(std::move(metrics));
  }

  for (const auto &[name, value] : ScoreGradedRuns(runs, grade)) {
    outcome.metrics[name] = value;
  }
  if (outcome.metrics.empty()) {
    outcome.error =
        "the graded command reported none of this problem's metrics";
    return outcome;
  }
  outcome.games_played = repeats;
  return outcome;
}

auto DockerBackend::MergedDir(int slot) const -> std::filesystem::path {
  return SlotDir(slot) / "merged";
}

auto DockerBackend::MountOverlay(int slot, std::string *error) -> bool {
  if (!config_.host_overlay) {
    return true;  // the container will assemble it, with CAP_SYS_ADMIN
  }
  UnmountOverlay(slot);  // a worker killed mid-order leaves one behind

  std::error_code ec;
  const auto upper = OverlayDir(slot) / "upper";
  const auto work = OverlayDir(slot) / "work";
  std::filesystem::create_directories(upper, ec);
  std::filesystem::create_directories(work, ec);
  std::filesystem::create_directories(MergedDir(slot), ec);
  if (ec) {
    *error = "cannot create overlay directories: " + ec.message();
    return false;
  }

  const std::string options = "lowerdir=" + RepoDir(slot).string() +
                              ",upperdir=" + upper.string() +
                              ",workdir=" + work.string();
  process::RunOptions run;
  run.timeout = std::chrono::seconds(60);
  run.stderr_path = SlotDir(slot) / "logs" / "mount.err";
  const process::RunResult mounted = process::RunCommand(
      config_.mount,
      {"-t", "overlay", "overlay", "-o", options, MergedDir(slot).string()},
      run);
  if (!mounted.started || mounted.exit_code != 0) {
    // No fallback to the in-container mount on purpose. That form needs
    // CAP_SYS_ADMIN, and quietly running submitted build code with it because a
    // mount failed is exactly the kind of downgrade nobody notices.
    *error =
        "cannot mount the slot overlay on the host: " +
        TailOf(ReadFile(run.stderr_path), 500) +
        " -- the worker needs to be able to mount overlayfs (root, or a user "
        "namespace). Set sandbox.host_overlay to false to use the in-container "
        "mount instead, which requires CAP_SYS_ADMIN and is not a boundary";
    return false;
  }
  return true;
}

void DockerBackend::UnmountOverlay(int slot) {
  if (!config_.host_overlay) {
    return;
  }
  process::RunOptions run;
  run.timeout = std::chrono::seconds(60);
  process::RunCommand(config_.umount, {MergedDir(slot).string()}, run);
}

auto DockerBackend::HardeningArgs() const -> std::vector<std::string> {
  std::vector<std::string> args;
  if (config_.host_overlay) {
    // Nothing left to mount, so nothing to be privileged for.
    args.insert(args.end(),
                {"--cap-drop", "ALL", "--security-opt", "no-new-privileges"});
    // Writable only where it must be. The overlay merge and the output base are
    // bind mounts and stay writable; everything else is not.
    args.emplace_back("--read-only");
    args.insert(args.end(), {"--tmpfs", "/tmp:exec"});
  } else {
    args.insert(args.end(), {"--cap-add", "SYS_ADMIN"});
  }
  if (!config_.run_as_user.empty()) {
    args.insert(args.end(), {"--user", config_.run_as_user});
  }
  if (config_.memory_limit_mb > 0) {
    args.insert(args.end(),
                {"--memory", std::to_string(config_.memory_limit_mb) + "m"});
  }
  if (config_.cpus > 0.0) {
    args.insert(args.end(), {"--cpus", std::to_string(config_.cpus)});
  }
  if (config_.pids_limit > 0) {
    args.insert(args.end(),
                {"--pids-limit", std::to_string(config_.pids_limit)});
  }
  return args;
}

auto DockerBackend::WorkspaceMounts(int slot) const
    -> std::vector<std::string> {
  if (config_.host_overlay) {
    // The merged tree is already assembled; the container just gets it.
    return {
        BindMount(MergedDir(slot), sandbox_common::kWorkspace, false),
        BindMount(OverlayDir(slot), sandbox_common::kScratch, false),
        BindMount(OutputBase(slot), sandbox_common::kOutputBaseMount, false)};
  }
  return {BindMount(RepoDir(slot), sandbox_common::kLowerMount, true),
          BindMount(OverlayDir(slot), sandbox_common::kScratch, false),
          BindMount(OutputBase(slot), sandbox_common::kOutputBaseMount, false)};
}

auto ContainerNameBase(int slot, const std::string &order_id) -> std::string {
  return "saw-" + std::to_string(slot) + "-" +
         sandbox_common::SanitizeContainerName(order_id);
}

void DockerBackend::Cancel(const std::string &order_id) {
  int slot = -1;
  {
    std::lock_guard lock(mutex_);
    const auto it = running_slots_.find(order_id);
    if (it == running_slots_.end()) {
      return;  // queued, or already finished
    }
    slot = it->second;
  }

  // Every container name is derived, so there is nothing to have remembered.
  // Killing one that already exited is a no-op, which is exactly the race we
  // want here rather than a lock held across a docker call.
  const std::string base = ContainerNameBase(slot, order_id);
  for (const char *suffix :
       {"-build", "-referee", "-bot", "-opponent", "-grade"}) {
    sandbox_common::KillContainer(config_.docker, base + suffix);
  }
  LOG(INFO) << "Cancelled order " << order_id << " (containers " << base
            << "-*)";
}

}  // namespace tournament_arena
