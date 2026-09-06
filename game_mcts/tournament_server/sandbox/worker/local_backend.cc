#include "game_mcts/tournament_server/sandbox/worker/local_backend.h"

#include <signal.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <csignal>
#include <fstream>
#include <functional>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "absl/log/log.h"
#include "game_mcts/common/process/process.h"
#include "game_mcts/tournament_server/sandbox/worker/bot_launch.h"
#include "game_mcts/tournament_server/sandbox/worker/build_log.h"
#include "game_mcts/tournament_server/sandbox/worker/metric_report.h"

namespace tournament_arena {

namespace {

// The caller's environment plus |extra|. RunOptions treats an empty env as
// "inherit", so adding one variable means rebuilding the whole list.
auto InheritedEnvWith(const std::string &extra) -> std::vector<std::string> {
  std::vector<std::string> env;
  for (char **entry = ::environ; entry != nullptr && *entry != nullptr;
       ++entry) {
    env.emplace_back(*entry);
  }
  env.push_back(extra);
  return env;
}

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
             std::size_t address_space_limit_bytes = 0,
             const std::function<void(pid_t)> &on_started = {}) -> StepResult {
  process::RunOptions options;
  options.on_started = on_started;
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

// Polls for the referee's port file. Polling rather than a pipe because the
// referee is started detached and its stdout is a log, not a channel.
auto AwaitPort(const std::filesystem::path &port_file,
               std::chrono::seconds limit) -> int {
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
    const StepResult clone = RunStep(
        config_.git, {"clone", "--local", config_.repo_url, repo.string()},
        SlotDir(slot), logs, "clone", std::chrono::seconds(900));
    if (!clone.run.started || clone.run.exit_code != 0) {
      *error = "git clone failed: " + TailOf(clone.output, 2000);
      return false;
    }
  }

  // Discard whatever the previous order's patch left behind. Not scoped to a
  // candidate directory any more: a patch may touch anything the problem's
  // policy allows, so anything may need undoing.
  //
  // Without -x on purpose. Ignored files are the slot's warm state -- bazel's
  // convenience symlinks and caches -- and removing them turns every order
  // into a cold build.
  const StepResult clean = RunStep(config_.git, {"clean", "-fd"}, repo, logs,
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
      *error = "git checkout " + base_commit +
               " failed: " + TailOf(checkout.output, 2000);
      return false;
    }
  }
  return true;
}

auto LocalBackend::ApplySide(int slot, const proto::Side &side,
                             std::string *error) -> bool {
  if (side.patch().empty()) {
    *error = "submission " + side.candidate_id() + " carries no patch";
    return false;
  }
  const std::filesystem::path repo = RepoDir(slot);
  const std::filesystem::path logs = SlotDir(slot) / "logs";
  const std::filesystem::path patch_file =
      SlotDir(slot) / (side.candidate_id() + ".diff");
  {
    std::ofstream out(patch_file, std::ios::binary | std::ios::trunc);
    if (!out) {
      *error = "cannot stage the patch for " + side.candidate_id();
      return false;
    }
    out.write(side.patch().data(),
              static_cast<std::streamsize>(side.patch().size()));
    if (!out) {
      *error = "short write staging the patch for " + side.candidate_id();
      return false;
    }
  }

  // --check first, so a patch that cannot apply is reported as itself rather
  // than as whatever the half-applied tree fails to build into.
  const StepResult check = RunStep(
      config_.git, {"apply", "--check", patch_file.string()}, repo, logs,
      "apply_check_" + side.candidate_id(), std::chrono::seconds(120));
  if (!check.run.started) {
    *error = "cannot run git";
    return false;
  }
  if (check.run.exit_code != 0) {
    *error = "patch for " + side.candidate_id() +
             " does not apply to this tree: " + TailOf(check.output, 1500);
    return false;
  }

  const StepResult apply =
      RunStep(config_.git, {"apply", patch_file.string()}, repo, logs,
              "apply_" + side.candidate_id(), std::chrono::seconds(120));
  if (apply.run.exit_code != 0) {
    *error = "git apply failed for " + side.candidate_id() + ": " +
             TailOf(apply.output, 1500);
    return false;
  }
  return true;
}

auto LocalBackend::Track(const std::string &order_id)
    -> std::function<void(pid_t)> {
  return [this, order_id](pid_t pgid) {
    std::lock_guard lock(mutex_);
    running_[order_id] = pgid;
  };
}

void LocalBackend::Untrack(const std::string &order_id) {
  std::lock_guard lock(mutex_);
  running_.erase(order_id);
}

namespace {

// Clears an order's tracked process group on every exit path.
struct UntrackGuard {
  LocalBackend *backend;
  std::string order_id;
  ~UntrackGuard() { backend->Untrack(order_id); }
};

}  // namespace

auto LocalBackend::RunOrder(int slot,
                            const proto::WorkOrder &order) -> OrderOutcome {
  OrderOutcome outcome;
  // Every step of this order publishes its process group while it runs, so a
  // Cancel arriving from the stream thread can reach work already under way.
  const auto track = Track(order.order_id());
  const UntrackGuard untrack{this, order.order_id()};
  const std::filesystem::path repo = RepoDir(slot);
  const std::filesystem::path logs = SlotDir(slot) / "logs";

  if (order.require_container()) {
    // The problem said its submissions need a container, and this backend is
    // not one: a submitted genrule here runs as this worker's own user. Better
    // to hand the order back than to quietly run it.
    outcome.error =
        "this problem requires a container; run the worker with "
        "--backend=docker";
    return outcome;
  }

  std::string error;
  if (!PrepareCheckout(slot, order.base_commit(), &error) ||
      !ApplySide(slot, order.candidate(), &error)) {
    outcome.error = error;
    return outcome;
  }
  // Both sides land in the same checkout, so a match problem's submissions must
  // touch disjoint paths. A conflict is reported as itself rather than as a
  // mysterious build failure.
  if (order.has_opponent() && !ApplySide(slot, order.opponent(), &error)) {
    outcome.error = error;
    outcome.build_failed_candidate_id = order.opponent().candidate_id();
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
  for (const std::string &target : order.candidate().build_targets()) {
    build_args.push_back(target);
  }
  if (order.has_opponent()) {
    for (const std::string &target : order.opponent().build_targets()) {
      build_args.push_back(target);
    }
  }
  if (!order.referee_target().empty()) {
    build_args.push_back(order.referee_target());
  }

  const StepResult build =
      RunStep(config_.bazel, build_args, repo, logs, "build",
              std::chrono::seconds(
                  order.build_timeout_s() > 0 ? order.build_timeout_s() : 1800),
              /*address_space_limit_bytes=*/0, track);
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

  const int run_timeout_s =
      order.run_timeout_s() > 0 ? order.run_timeout_s() : 1800;

  if (order.has_grade()) {
    return RunGrade(slot, order, std::move(outcome));
  }

  // The match runs here, not against a central broker: the worker starts a
  // referee beside the bots and reads its tally. That is what lets the
  // coordinator link no game code -- see referee/main.cc.
  const std::filesystem::path bin = repo / "bazel-bin";
  const std::filesystem::path referee =
      bin / std::filesystem::path(BinaryPathForTarget(order.referee_target()));
  const std::filesystem::path port_file = logs / "referee.port";
  std::filesystem::remove(port_file);

  const int match_deadline_s = order.match_deadline_s() > 0
                                   ? order.match_deadline_s()
                                   : std::max(1, run_timeout_s - 30);
  std::vector<std::string> referee_args = {
      "--port=0",
      "--port_file=" + port_file.string(),
      "--game=" + order.game(),
      "--games=" + std::to_string(order.num_games()),
      "--player_a=" + order.candidate().candidate_id(),
      "--player_b=" + order.opponent_spec(),
      "--scratch_dir=" + (SlotDir(slot) / "referee").string(),
      "--deadline_s=" + std::to_string(match_deadline_s),
  };
  const std::filesystem::path referee_out = logs / "referee.out";
  const std::filesystem::path referee_err = logs / "referee.err";
  if (!std::filesystem::exists(referee)) {
    outcome.error = "referee binary is missing at " + referee.string() +
                    " (built from " + order.referee_target() + ")";
    return outcome;
  }
  process::InputStreamProcess referee_proc = process::CreateInputStreamProcess(
      referee.string(), referee_args, /*env=*/{}, referee_out, referee_err);

  // The port is only knowable once the referee is listening. Waiting for the
  // file it writes after bind() is the difference between "the bot could not
  // connect" and "the bot connected before anything was there".
  const int port = AwaitPort(port_file, std::chrono::seconds(60));
  if (port <= 0) {
    referee_proc.Wait();
    outcome.error =
        "referee never reported a port: " + TailOf(ReadFile(referee_err), 1500);
    return outcome;
  }
  const std::string target = "localhost:" + std::to_string(port);

  // The opponent bot, when there is one, plays for the whole match while the
  // primary bot runs in the foreground. Both stop when the referee closes their
  // streams, which is what bounds them: the referee's own --deadline_s is the
  // real clock here.
  std::optional<process::InputStreamProcess> opponent_proc;
  if (order.has_opponent()) {
    opponent_proc = process::CreateInputStreamProcess(
        (bin / BinaryPathForTarget(order.opponent().bot_target())).string(),
        BotArgs(order.opponent().candidate_id(), target,
                std::string(kPlayerPrefix) + order.candidate().candidate_id(),
                order.num_games(), FormatParams(order.opponent().params())),
        /*env=*/{}, logs / "opponent.out", logs / "opponent.err");
  }

  const std::filesystem::path bot =
      bin / BinaryPathForTarget(order.candidate().bot_target());
  const StepResult run = RunStep(
      bot.string(),
      BotArgs(order.candidate().candidate_id(), target, order.opponent_spec(),
              order.num_games(), FormatParams(order.candidate().params())),
      repo, logs, "run", std::chrono::seconds(run_timeout_s),
      config_.memory_limit_mb > 0
          ? static_cast<std::size_t>(config_.memory_limit_mb) * 1024 * 1024
          : 0);

  if (opponent_proc.has_value()) {
    opponent_proc->Wait();
  }
  referee_proc.Wait();

  if (!run.run.started) {
    outcome.error = "built bot is missing at " + bot.string();
    return outcome;
  }

  // The referee's tally, not the bot's. The bot only knows what it was told;
  // the referee applied every move and is the one that decided the games.
  RunTally tally;
  const std::string referee_output = ReadFile(referee_out);
  if (!ParseResultLine(referee_output, &tally)) {
    outcome.error =
        "referee produced no result" +
        std::string(run.run.timed_out ? " (the bot timed out first)" : "") +
        ": " + TailOf(ReadFile(referee_err) + referee_output, 1500);
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

auto LocalBackend::RunGrade(int slot, const proto::WorkOrder &order,
                            OrderOutcome outcome) -> OrderOutcome {
  const std::filesystem::path repo = RepoDir(slot);
  const std::filesystem::path logs = SlotDir(slot) / "logs";
  const proto::GradeOrder &grade = order.grade();
  const auto track = Track(order.order_id());

  if (grade.argv().empty()) {
    outcome.error = "graded order carries no command to run";
    return outcome;
  }
  const int repeats = std::max(1, grade.repeats());
  const int timeout_s = grade.timeout_s() > 0 ? grade.timeout_s() : 1800;

  std::vector<std::string> args(grade.argv().begin() + 1, grade.argv().end());
  const std::string executable = grade.argv(0);

  std::vector<std::map<std::string, double>> runs;
  for (int run = 0; run < repeats; ++run) {
    const std::filesystem::path report =
        logs / ("report_" + std::to_string(run) + ".json");
    std::filesystem::remove(report);

    process::RunOptions options;
    options.cwd = repo;
    options.stdout_path = logs / ("grade_" + std::to_string(run) + ".out");
    options.stderr_path = logs / ("grade_" + std::to_string(run) + ".err");
    options.timeout = std::chrono::seconds(timeout_s);
    options.address_space_limit_bytes =
        config_.memory_limit_mb > 0
            ? static_cast<std::size_t>(config_.memory_limit_mb) * 1024 * 1024
            : 0;
    // The command writes its numbers here. Passed rather than fixed so the
    // command needs no knowledge of the worker's directory layout.
    options.env = InheritedEnvWith("ARENA_REPORT=" + report.string());
    options.on_started = track;

    const process::RunResult result =
        process::RunCommand(executable, args, options);
    const std::string output = ReadFile(options.stdout_path);
    if (!result.started) {
      outcome.error = "cannot run the graded command '" + executable + "'";
      return outcome;
    }
    if (result.timed_out) {
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
    if (!ParseMetricReport(ReadFile(report), output, &metrics)) {
      outcome.error =
          "graded run produced no metrics: write JSON to $ARENA_REPORT or "
          "print a RESULT line. Output was: " +
          TailOf(output, 1000);
      return outcome;
    }
    runs.push_back(std::move(metrics));
  }

  const auto aggregated = AggregateMetrics(runs, grade.aggregate());
  // Only what the problem ranks on is kept. A benchmark printing more is
  // normal; storing it all would let a report grow the standings file without
  // bound.
  for (const std::string &name : grade.metric_names()) {
    const auto it = aggregated.find(name);
    if (it != aggregated.end()) {
      outcome.metrics[name] = it->second;
    }
  }
  if (outcome.metrics.empty()) {
    outcome.error =
        "the graded command reported none of this problem's metrics";
    return outcome;
  }
  outcome.games_played = repeats;  // runs, for a graded order
  return outcome;
}

void LocalBackend::Cancel(const std::string &order_id) {
  pid_t pgid = 0;
  {
    std::lock_guard lock(mutex_);
    const auto it = running_.find(order_id);
    if (it == running_.end()) {
      return;  // queued, between steps, or already finished
    }
    pgid = it->second;
  }
  // The whole group: bazel spawns a tree, and killing only the parent leaves
  // the workers building. Racing a step that just exited is harmless -- the
  // group is gone and killpg fails, which is the outcome we wanted anyway.
  if (pgid > 0) {
    ::killpg(pgid, SIGKILL);
    LOG(INFO) << "Cancelled order " << order_id << " (process group " << pgid
              << ")";
  }
}

}  // namespace tournament_arena
