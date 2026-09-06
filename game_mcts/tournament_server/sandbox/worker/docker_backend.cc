#include "game_mcts/tournament_server/sandbox/worker/docker_backend.h"

#include <cctype>
#include <fstream>
#include <sstream>
#include <system_error>
#include <utility>

#include "absl/log/log.h"
#include "game_mcts/common/process/process.h"
#include "game_mcts/tournament_server/sandbox/worker/build_log.h"
#include "game_mcts/tournament_server/sandbox/worker/candidate_build.h"

namespace tournament_arena {

namespace {

auto ReadFile(const std::filesystem::path &path) -> std::string {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return {};
  }
  return std::string(std::istreambuf_iterator<char>(in),
                     std::istreambuf_iterator<char>());
}

// One subprocess step with its output captured to a file, the same shape the
// local backend uses. stdout and stderr stay apart and are concatenated only
// when reporting.
struct StepResult {
  process::RunResult run;
  std::string output;
};

auto RunStep(const std::string &executable,
             const std::vector<std::string> &args,
             const std::filesystem::path &cwd,
             const std::filesystem::path &log_dir, const std::string &tag,
             std::chrono::seconds timeout) -> StepResult {
  process::RunOptions options;
  options.cwd = cwd;
  options.stdout_path = log_dir / (tag + ".out");
  options.stderr_path = log_dir / (tag + ".err");
  options.timeout = timeout;

  StepResult result;
  result.run = process::RunCommand(executable, args, options);
  result.output = ReadFile(options.stdout_path) + ReadFile(options.stderr_path);
  return result;
}

// A bind mount in `docker run --mount` syntax. The long form is deliberate:
// `-v` creates |source| as an empty directory when it does not exist, which
// would turn a missing slot directory into an empty repository or a run that
// silently drops every patch. --mount fails the run instead.
auto BindMount(const std::filesystem::path &source, const std::string &target,
               bool readonly) -> std::string {
  std::string mount = "type=bind,source=" + source.string() +
                      ",target=" + target;
  if (readonly) {
    mount += ",readonly";
  }
  return mount;
}

// The overlay assembly shared by both container scripts. The upper and work
// dirs sit under one bind mount so they are guaranteed to share a filesystem,
// which overlayfs requires.
auto OverlayMountScript() -> std::string {
  return "mkdir -p " + std::string(kDockerScratch) + "/upper " +
         kDockerScratch + "/work " + kDockerWorkspace + "\n"
         "export HOME=" + kDockerScratch + "\n"
         "mount -t overlay overlay -o lowerdir=" + kDockerLowerMount +
         ",upperdir=" + kDockerScratch + "/upper,workdir=" + kDockerScratch +
         "/work " + kDockerWorkspace + "\n";
}

}  // namespace

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

auto SanitizeContainerName(const std::string &value) -> std::string {
  std::string name;
  for (const unsigned char c : value) {
    if (std::isalnum(c) != 0 || c == '_' || c == '.' || c == '-') {
      name += static_cast<char>(c);
    } else {
      name += '-';
    }
  }
  return name;
}

auto DockerBuildScript(const std::filesystem::path &disk_cache,
                       const std::vector<std::string> &bazel_flags,
                       const std::string &target) -> std::string {
  std::string script = "set -eu\n";
  script += OverlayMountScript();
  script += "cd " + std::string(kDockerWorkspace) + "\n";
  script += "cp -a " + std::string(kDockerPatchMount) + "/. " +
            kDockerWorkspace + "/\n";
  script += std::string("exec bazel --output_base=") + kDockerOutputBaseMount;
  if (!disk_cache.empty()) {
    script += std::string(" --disk_cache=") + kDockerDiskCacheMount;
  }
  for (const std::string &flag : bazel_flags) {
    script += " " + ShellQuote(flag);
  }
  script += " build " + ShellQuote(target) + "\n";
  return script;
}

auto DockerRunScript(const std::string &bot_path,
                     const std::vector<std::string> &args) -> std::string {
  std::string script = "set -eu\n";
  script += OverlayMountScript();
  script += "cd " + std::string(kDockerWorkspace) + "\n";
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
    if (!CloneSlot(slot, error)) {
      return false;
    }
  }
  return true;
}

auto DockerBackend::CloneSlot(int slot, std::string *error) -> bool {
  const std::filesystem::path repo = RepoDir(slot);
  const std::filesystem::path logs = SlotDir(slot) / "logs";
  std::error_code ec;
  std::filesystem::create_directories(logs, ec);

  if (!std::filesystem::exists(repo / ".git")) {
    std::filesystem::create_directories(SlotDir(slot), ec);
    // --local hardlinks the object store instead of copying it, so a clone of
    // a multi-gigabyte history costs almost nothing on the same filesystem.
    const StepResult clone =
        RunStep(config_.git, {"clone", "--local", config_.repo_dir.string(),
                              repo.string()},
                SlotDir(slot), logs, "clone", std::chrono::seconds(900));
    if (!clone.run.started || clone.run.exit_code != 0) {
      *error = "git clone failed: " + TailOf(clone.output, 2000);
      return false;
    }
  }
  return true;
}

auto DockerBackend::PrepareCheckout(int slot, const std::string &base_commit,
                                    std::string *error) -> bool {
  if (!CloneSlot(slot, error)) {
    return false;
  }

  const std::filesystem::path repo = RepoDir(slot);
  const std::filesystem::path logs = SlotDir(slot) / "logs";
  if (base_commit.empty()) {
    return true;
  }

  const StepResult fetch = RunStep(
      config_.git, {"fetch", "--all", "--tags", "--quiet"}, repo, logs,
      "fetch", std::chrono::seconds(600));
  (void)fetch;  // A stale mirror is survivable; the checkout below decides.

  const StepResult checkout =
      RunStep(config_.git, {"checkout", "--force", base_commit}, repo, logs,
              "checkout", std::chrono::seconds(300));
  if (!checkout.run.started || checkout.run.exit_code != 0) {
    *error = "git checkout " + base_commit + " failed: " +
             TailOf(checkout.output, 2000);
    return false;
  }
  return true;
}

auto DockerBackend::StageCandidate(int slot, const proto::WorkOrder &order,
                                   std::string *error) -> bool {
  // The previous order's candidate lives in the slot's overlay upper, not in
  // the clone: clear it here so the merged tree carries exactly this
  // submission. Removing an upper file that the lower also has re-exposes the
  // lower's version, which is harmless because every file of this order is
  // copied back over the merge below, entry header included.
  std::error_code ec;
  std::filesystem::remove_all(
      OverlayDir(slot) / "upper" / kCandidateDir / order.candidate_id(), ec);

  const std::filesystem::path candidate_root =
      PatchDir(slot) / kCandidateDir / order.candidate_id();
  std::filesystem::remove_all(PatchDir(slot), ec);
  std::filesystem::create_directories(candidate_root, ec);
  if (ec) {
    *error = "cannot create patch directory " +
             PatchDir(slot).string() + ": " + ec.message();
    return false;
  }

  for (const proto::SourceFile &file : order.files()) {
    // The arena validated these paths at submit time; re-checking the result
    // here is cheap insurance against a malformed order from anywhere else.
    const std::filesystem::path rel(file.path());
    if (rel.is_absolute()) {
      *error = "candidate file '" + file.path() + "' must be relative";
      return false;
    }
    for (const auto &component : rel) {
      if (component == "." || component == "..") {
        *error = "candidate file '" + file.path() +
                 "' escapes its directory";
        return false;
      }
    }
    const std::filesystem::path path = candidate_root / rel;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
      *error = "cannot write '" + file.path() + "'";
      return false;
    }
    out.write(file.content().data(),
              static_cast<std::streamsize>(file.content().size()));
    if (!out) {
      *error = "short write for '" + file.path() + "'";
      return false;
    }
  }

  const std::string build_file = GenerateCandidateBuild(order);
  if (build_file.empty()) {
    *error = "cannot generate a BUILD file for this order (unknown game '" +
             order.game() + "' or missing entry header)";
    return false;
  }
  std::ofstream build(candidate_root / "BUILD", std::ios::trunc);
  if (!build) {
    *error = "cannot write the generated BUILD file";
    return false;
  }
  build << build_file;
  return build.good();
}

auto DockerBackend::RunOrder(int slot, const proto::WorkOrder &order)
    -> OrderOutcome {
  OrderOutcome outcome;
  const std::filesystem::path logs = SlotDir(slot) / "logs";

  std::string error;
  if (!PrepareCheckout(slot, order.base_commit(), &error) ||
      !StageCandidate(slot, order, &error)) {
    outcome.error = error;
    return outcome;
  }

  const std::string name_base =
      "saw-" + std::to_string(slot) + "-" +
      SanitizeContainerName(order.order_id());

  // --network=host on both containers: the bot dials the broker at an address
  // the host resolves, and a cold bazel may still need to reach the module
  // cache. This is why the backend is isolation, not a security boundary.
  std::vector<std::string> build_mounts = {
      BindMount(RepoDir(slot), kDockerLowerMount, true),
      BindMount(OverlayDir(slot), kDockerScratch, false),
      BindMount(PatchDir(slot), kDockerPatchMount, true),
      BindMount(OutputBase(slot), kDockerOutputBaseMount, false),
  };
  if (!config_.disk_cache.empty()) {
    build_mounts.push_back(BindMount(config_.disk_cache, kDockerDiskCacheMount,
                                     false));
  }
  const std::string build_script = DockerBuildScript(
      config_.disk_cache, config_.bazel_flags,
      CandidateTarget(order.candidate_id()));
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
    process::RunOptions rm;
    rm.timeout = std::chrono::seconds(60);
    process::RunCommand(config_.docker, {"rm", "-f", build_name}, rm);

    std::vector<std::string> args = {"run", "--rm", "--name", build_name,
                                     "--cap-add", "SYS_ADMIN",
                                     "--network", "host"};
    for (const std::string &mount : build_mounts) {
      args.push_back("--mount");
      args.push_back(mount);
    }
    args.push_back("--entrypoint");
    args.push_back("/bin/sh");
    args.push_back(config_.docker_image);
    args.push_back("-c");
    args.push_back(build_script);

    const process::RunResult build =
        process::RunCommand(config_.docker, args, options);
    const std::string build_output =
        ReadFile(options.stdout_path) + ReadFile(options.stderr_path);
    if (!build.started) {
      outcome.error = "cannot run docker ('" + config_.docker +
                      "' not found)";
      return outcome;
    }
    if (build.timed_out) {
      // The timeout killed the docker *client*; the container is managed by
      // the daemon and would otherwise keep building.
      process::RunOptions kill;
      kill.timeout = std::chrono::seconds(60);
      process::RunCommand(config_.docker, {"kill", build_name}, kill);
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

  std::vector<std::string> run_args = {
      "--name=" + order.candidate_id(),
      "--server=" + order.broker_target(),
      "--opponent=" + order.opponent(),
      "--games=" + std::to_string(order.num_games()),
  };
  const std::string params = FormatParams(order);
  if (!params.empty()) {
    run_args.push_back("--params=" + params);
  }

  const std::string run_name = name_base + "-run";
  std::vector<std::string> run_mounts = {
      BindMount(RepoDir(slot), kDockerLowerMount, true),
      BindMount(OverlayDir(slot), kDockerScratch, false),
      BindMount(OutputBase(slot), kDockerOutputBaseMount, false),
  };
  std::vector<std::string> run_extra;
  if (config_.memory_limit_mb > 0) {
    // The cap goes on the bot's container, mirroring the local backend's
    // address-space limit: an over-allocating candidate fails its own
    // allocation rather than pushing the host into swap.
    run_extra.push_back("--memory");
    run_extra.push_back(std::to_string(config_.memory_limit_mb) + "m");
  }
  const std::string bot_path =
      "./bazel-bin/" + std::string(kCandidateDir) + "/" +
      order.candidate_id() + "/bot";
  {
    const std::chrono::seconds run_timeout = std::chrono::seconds(
        order.run_timeout_s() > 0 ? order.run_timeout_s() : 1800);
    process::RunOptions options;
    options.stdout_path = logs / (run_name + ".out");
    options.stderr_path = logs / (run_name + ".err");
    options.timeout = run_timeout;

    process::RunOptions rm;
    rm.timeout = std::chrono::seconds(60);
    process::RunCommand(config_.docker, {"rm", "-f", run_name}, rm);

    std::vector<std::string> args = {"run", "--rm", "--name", run_name,
                                     "--cap-add", "SYS_ADMIN",
                                     "--network", "host"};
    for (const std::string &mount : run_mounts) {
      args.push_back("--mount");
      args.push_back(mount);
    }
    for (const std::string &extra : run_extra) {
      args.push_back(extra);
    }
    args.push_back("--entrypoint");
    args.push_back("/bin/sh");
    args.push_back(config_.docker_image);
    args.push_back("-c");
    args.push_back(DockerRunScript(bot_path, run_args));

    const process::RunResult run =
        process::RunCommand(config_.docker, args, options);
    const std::string run_output =
        ReadFile(options.stdout_path) + ReadFile(options.stderr_path);
    if (!run.started) {
      outcome.error = "cannot run docker ('" + config_.docker +
                      "' not found)";
      return outcome;
    }
    if (run.timed_out) {
      process::RunOptions kill;
      kill.timeout = std::chrono::seconds(60);
      process::RunCommand(config_.docker, {"kill", run_name}, kill);
      outcome.error = "games timed out after " +
                      std::to_string(run_timeout.count()) + "s";
      return outcome;
    }

    RunTally tally;
    if (!ParseResultLine(run_output, &tally)) {
      // No RESULT line means the bot died before finishing -- a crash, or a
      // rendezvous that never paired. Either way there is nothing to record.
      outcome.error = "bot produced no result (exit " +
                      std::to_string(run.exit_code) + "): " +
                      TailOf(run_output, 1500);
      return outcome;
    }
    outcome.games_played = tally.games;
    outcome.wins = tally.wins;
    outcome.draws = tally.draws;
    outcome.losses = tally.losses;
    outcome.elo = tally.elo;
  }
  return outcome;
}

}  // namespace tournament_arena
