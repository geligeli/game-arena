#include "cpp/tournament_server/sandbox_worker/local_backend.h"

#include <fstream>
#include <sstream>
#include <utility>

#include "absl/log/log.h"
#include "game_mcts/cpp/process/process.h"
#include "cpp/tournament_server/sandbox_worker/build_log.h"
#include "cpp/tournament_server/sandbox_worker/candidate_build.h"

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

// One step of an order, with its output captured to a file. Combining stdout
// and stderr into one file would interleave unpredictably, so they stay apart
// and are concatenated only when reporting.
struct StepResult {
  process::RunResult run;
  std::string output;
};

auto RunStep(const std::string &executable,
             const std::vector<std::string> &args,
             const std::filesystem::path &cwd,
             const std::filesystem::path &log_dir, const std::string &tag,
             std::chrono::seconds timeout,
             std::size_t address_space_limit_bytes = 0) -> StepResult {
  process::RunOptions options;
  options.cwd = cwd;
  options.stdout_path = log_dir / (tag + ".out");
  options.stderr_path = log_dir / (tag + ".err");
  options.timeout = timeout;
  options.address_space_limit_bytes = address_space_limit_bytes;

  StepResult result;
  result.run = process::RunCommand(executable, args, options);
  result.output = ReadFile(options.stdout_path) + ReadFile(options.stderr_path);
  return result;
}

}  // namespace

LocalBackend::LocalBackend(LocalBackendConfig config)
    : config_(std::move(config)) {}

auto LocalBackend::SlotDir(int slot) const -> std::filesystem::path {
  return config_.work_dir / ("slot" + std::to_string(slot));
}

auto LocalBackend::RepoDir(int slot) const -> std::filesystem::path {
  return SlotDir(slot) / "repo";
}

auto LocalBackend::OutputBase(int slot) const -> std::filesystem::path {
  return SlotDir(slot) / "bazel_output_base";
}

auto LocalBackend::Warmup(int slots, std::string *error) -> bool {
  for (int slot = 0; slot < slots; ++slot) {
    if (!PrepareCheckout(slot, /*base_commit=*/"", error)) {
      return false;
    }
  }
  return true;
}

auto LocalBackend::PrepareCheckout(int slot, const std::string &base_commit,
                                   std::string *error) -> bool {
  const std::filesystem::path repo = RepoDir(slot);
  const std::filesystem::path logs = SlotDir(slot) / "logs";
  std::error_code ec;
  std::filesystem::create_directories(logs, ec);

  if (!std::filesystem::exists(repo / ".git")) {
    std::filesystem::create_directories(SlotDir(slot), ec);
    // --local hardlinks the object store instead of copying it, so a clone of
    // a multi-gigabyte history costs almost nothing on the same filesystem.
    const StepResult clone =
        RunStep(config_.git, {"clone", "--local", config_.repo_url,
                              repo.string()},
                SlotDir(slot), logs, "clone", std::chrono::seconds(900));
    if (!clone.run.started || clone.run.exit_code != 0) {
      *error = "git clone failed: " + TailOf(clone.output, 2000);
      return false;
    }
  }

  // Discard whatever the previous order left behind before checking out, or a
  // stale candidate directory blocks the checkout and then gets built again.
  const StepResult clean =
      RunStep(config_.git, {"clean", "-xfd", kCandidateDir}, repo, logs,
              "clean", std::chrono::seconds(120));
  if (!clean.run.started) {
    *error = "cannot run git";
    return false;
  }

  if (!base_commit.empty()) {
    const StepResult fetch =
        RunStep(config_.git, {"fetch", "--all", "--tags", "--quiet"}, repo,
                logs, "fetch", std::chrono::seconds(600));
    (void)fetch;  // A stale mirror is survivable; the checkout below decides.

    const StepResult checkout =
        RunStep(config_.git, {"checkout", "--force", base_commit}, repo, logs,
                "checkout", std::chrono::seconds(300));
    if (!checkout.run.started || checkout.run.exit_code != 0) {
      *error = "git checkout " + base_commit + " failed: " +
               TailOf(checkout.output, 2000);
      return false;
    }
  }
  return true;
}

auto LocalBackend::WriteCandidate(int slot, const proto::WorkOrder &order,
                                  std::string *error) -> bool {
  const std::filesystem::path root =
      RepoDir(slot) / kCandidateDir / order.candidate_id();
  std::error_code ec;
  std::filesystem::remove_all(root, ec);
  std::filesystem::create_directories(root, ec);
  if (ec) {
    *error = "cannot create candidate directory: " + ec.message();
    return false;
  }

  for (const proto::SourceFile &file : order.files()) {
    const std::filesystem::path path = root / file.path();
    // The arena validated these paths at submit time; re-checking the result
    // here is cheap insurance against a malformed order from anywhere else.
    const auto normalized = std::filesystem::weakly_canonical(path, ec);
    const auto root_canonical = std::filesystem::weakly_canonical(root, ec);
    if (normalized.string().rfind(root_canonical.string(), 0) != 0) {
      *error = "candidate file '" + file.path() + "' escapes its directory";
      return false;
    }
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
  std::ofstream build(root / "BUILD", std::ios::trunc);
  if (!build) {
    *error = "cannot write the generated BUILD file";
    return false;
  }
  build << build_file;
  return build.good();
}

auto LocalBackend::RunOrder(int slot, const proto::WorkOrder &order)
    -> OrderOutcome {
  OrderOutcome outcome;
  const std::filesystem::path repo = RepoDir(slot);
  const std::filesystem::path logs = SlotDir(slot) / "logs";

  std::string error;
  if (!PrepareCheckout(slot, order.base_commit(), &error) ||
      !WriteCandidate(slot, order, &error)) {
    outcome.error = error;
    return outcome;
  }

  // --output_base is per slot so parallel builds do not queue on bazel's
  // workspace lock; --disk_cache is shared so they still reuse each other's
  // artifacts.
  std::vector<std::string> build_args = {
      "--output_base=" + OutputBase(slot).string(), "build"};
  if (!config_.disk_cache.empty()) {
    build_args.push_back("--disk_cache=" + config_.disk_cache.string());
  }
  for (const std::string &flag : config_.bazel_flags) {
    build_args.push_back(flag);
  }
  build_args.push_back(CandidateTarget(order.candidate_id()));

  const StepResult build = RunStep(
      config_.bazel, build_args, repo, logs, "build",
      std::chrono::seconds(order.build_timeout_s() > 0 ? order.build_timeout_s()
                                                       : 1800));
  if (!build.run.started) {
    outcome.error = "cannot run bazel ('" + config_.bazel + "' not found)";
    return outcome;
  }
  if (build.run.timed_out) {
    outcome.build_log = CompactBuildLog(build.output);
    outcome.error = "build timed out after " +
                    std::to_string(order.build_timeout_s()) + "s";
    return outcome;
  }
  if (build.run.exit_code != 0) {
    // A build failure is the candidate's fault, not the order's: report it as
    // a completed order with build_ok=false so the agent gets the diagnostics.
    outcome.build_log = CompactBuildLog(build.output);
    return outcome;
  }
  outcome.build_ok = true;

  const std::filesystem::path bot =
      repo / "bazel-bin" / kCandidateDir / order.candidate_id() / "bot";
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

  // The cap goes on the bot, not on bazel: a candidate that allocates without
  // bound should fail its own allocation rather than push the host into swap.
  // Bazel legitimately needs more than any single bot does.
  const StepResult run = RunStep(
      bot.string(), run_args, repo, logs, "run",
      std::chrono::seconds(order.run_timeout_s() > 0 ? order.run_timeout_s()
                                                     : 1800),
      config_.memory_limit_mb > 0
          ? static_cast<std::size_t>(config_.memory_limit_mb) * 1024 * 1024
          : 0);
  if (!run.run.started) {
    outcome.error = "built bot is missing at " + bot.string();
    return outcome;
  }
  if (run.run.timed_out) {
    outcome.error = "games timed out after " +
                    std::to_string(order.run_timeout_s()) + "s";
    return outcome;
  }

  RunTally tally;
  if (!ParseResultLine(run.output, &tally)) {
    // No RESULT line means the bot died before finishing -- a crash, or a
    // rendezvous that never paired. Either way there is nothing to record.
    outcome.error = "bot produced no result (exit " +
                    std::to_string(run.run.exit_code) + "): " +
                    TailOf(run.output, 1500);
    return outcome;
  }
  outcome.games_played = tally.games;
  outcome.wins = tally.wins;
  outcome.draws = tally.draws;
  outcome.losses = tally.losses;
  outcome.elo = tally.elo;
  return outcome;
}

}  // namespace tournament_arena
