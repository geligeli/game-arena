#include "game_arena/sandbox/worker/order_outcome.h"

#include <map>
#include <string>
#include <utility>
#include <vector>

#include "game_arena/common/metric_report/metric_report.h"
#include "game_arena/sandbox/common/text.h"
#include "game_arena/sandbox/worker/build_log.h"
#include "game_arena/sandbox/worker/grade_policy.h"
#include "game_arena/sandbox/worker/match_tally.h"

namespace tournament_arena {

namespace sx = sandbox_exec::proto;

namespace {

using sandbox_common::TailOf;

auto StepNamed(const sx::PhaseResult &phase,
               const std::string &name) -> const sx::StepResult * {
  for (const sx::StepResult &step : phase.steps()) {
    if (step.name() == name) {
      return &step;
    }
  }
  return nullptr;
}

auto PhaseNamed(const sx::JobResult &result,
                const std::string &name) -> const sx::PhaseResult * {
  for (const sx::PhaseResult &phase : result.phases()) {
    if (phase.name() == name) {
      return &phase;
    }
  }
  return nullptr;
}

// The order's own candidate is the one being evaluated; the opponent's build
// breaking is somebody else's problem and must not retire this submission.
auto BlameForBuild(const proto::WorkOrder &order,
                   const std::string &log) -> std::string {
  if (!order.has_opponent()) {
    return "";
  }
  const std::string &opponent = order.opponent().candidate_id();
  if (!opponent.empty() && log.find(opponent) != std::string::npos &&
      log.find(order.candidate().candidate_id()) == std::string::npos) {
    return opponent;
  }
  return "";
}

auto ReadMatch(const proto::WorkOrder &order, const sx::PhaseResult &match,
               OrderOutcome *outcome) -> void {
  const sx::StepResult *referee = StepNamed(match, "referee");
  const sx::StepResult *bot = StepNamed(match, "bot");
  const std::string referee_output =
      referee != nullptr ? referee->stdout() : "";
  const std::string referee_errors =
      referee != nullptr ? referee->stderr() : "";

  // The referee's tally, not the bot's. The bot only knows what it was told;
  // the referee applied every move and is the one that decided the games.
  RunTally tally;
  if (!ParseResultLine(referee_output, &tally)) {
    outcome->error = "referee produced no result" +
                     std::string(bot != nullptr && bot->timed_out()
                                     ? " (the bot timed out first)"
                                     : "") +
                     ": " + TailOf(referee_errors + referee_output, 1500);
    return;
  }
  if (tally.games < order.num_games()) {
    // Recorded, not fatal: the games that were played are real results, and
    // an agent is better served by a short match plus the reason than by
    // nothing.
    outcome->error = "match was short: " + std::to_string(tally.games) +
                     " of " + std::to_string(order.num_games()) +
                     " games played";
  }
  outcome->games_played = tally.games;
  outcome->wins = tally.wins;
  outcome->draws = tally.draws;
  outcome->losses = tally.losses;
  outcome->elo = tally.elo;
}

auto ReadGrade(const proto::WorkOrder &order, const sx::JobResult &result,
               OrderOutcome *outcome) -> void {
  std::vector<std::map<std::string, double>> runs;
  for (const sx::PhaseResult &phase : result.phases()) {
    if (phase.name() != "grade") {
      continue;
    }
    const sx::StepResult *step = StepNamed(phase, "grade");
    if (step == nullptr) {
      continue;
    }
    if (step->timed_out()) {
      outcome->error = "graded run timed out after " +
                       std::to_string(step->timeout_s()) + "s";
      return;
    }
    if (step->exit_code() != 0) {
      // A number from a failed run looks like a result, which is worse than
      // no number at all.
      outcome->error = "graded command exited " +
                       std::to_string(step->exit_code()) + ": " +
                       TailOf(step->stderr() + step->stdout(), 1000);
      return;
    }
    std::map<std::string, double> metrics;
    const auto collected = step->collected().find("report.json");
    const std::string report =
        collected != step->collected().end() ? collected->second : "";
    if (!metric_report::Parse(report, step->stdout(), &metrics)) {
      outcome->error =
          "graded run produced no metrics: write JSON to $ARENA_REPORT or "
          "print a RESULT line. Output was: " +
          TailOf(step->stdout(), 1000);
      return;
    }
    runs.push_back(std::move(metrics));
  }

  for (const auto &[name, value] : ScoreGradedRuns(runs, order.grade())) {
    outcome->metrics[name] = value;
  }
  if (outcome->metrics.empty()) {
    outcome->error =
        "the graded command reported none of this problem's metrics";
    return;
  }
  // Runs, for a graded order: the same field a match fills with games.
  outcome->games_played = static_cast<int>(runs.size());
}

}  // namespace

auto OutcomeFor(const proto::WorkOrder &order,
                const sx::JobResult &result) -> OrderOutcome {
  OrderOutcome outcome;

  const sx::PhaseResult *build = PhaseNamed(result, "build");
  const sx::StepResult *build_step =
      build != nullptr ? StepNamed(*build, "build") : nullptr;

  // Whatever the engine says it could not do, before looking at any step: a
  // job that never ran has no result to read.
  if (result.status().code() != sx::Status::OK) {
    if (build_step != nullptr && !build_step->stdout().empty()) {
      outcome.build_log =
          CompactBuildLog(build_step->stdout() + build_step->stderr());
    }
    outcome.error = result.status().message();
    return outcome;
  }

  if (build_step == nullptr) {
    outcome.error = "the job reported no build";
    return outcome;
  }
  const std::string build_output = build_step->stdout() + build_step->stderr();
  if (build_step->timed_out()) {
    outcome.build_log = CompactBuildLog(build_output);
    // The timeout the engine actually enforced, not the one the order asked
    // for: an order that leaves build_timeout_s unset used to report "build
    // timed out after 0s".
    outcome.error = "build timed out after " +
                    std::to_string(build_step->timeout_s()) + "s";
    return outcome;
  }
  if (build_step->exit_code() != 0) {
    // A build failure is the candidate's fault, not the order's: report it as
    // a completed order with build_ok false so the agent gets the
    // diagnostics.
    outcome.build_log = CompactBuildLog(build_output);
    outcome.build_failed_candidate_id = BlameForBuild(order, build_output);
    return outcome;
  }
  outcome.build_ok = true;

  if (order.has_grade()) {
    ReadGrade(order, result, &outcome);
    return outcome;
  }

  const sx::PhaseResult *match = PhaseNamed(result, "match");
  if (match == nullptr) {
    outcome.error = "the job reported no match";
    return outcome;
  }
  const sx::StepResult *bot = StepNamed(*match, "bot");
  if (bot != nullptr && !bot->started()) {
    outcome.error = "the bot could not be started";
    return outcome;
  }
  if (bot != nullptr && bot->timed_out()) {
    // Named rather than reported as a missing referee result: the two
    // backends disagreed about this, and "games timed out after 2s" says what
    // happened while "referee produced no result" describes a symptom.
    outcome.error =
        "games timed out after " + std::to_string(bot->timeout_s()) + "s";
    return outcome;
  }
  ReadMatch(order, *match, &outcome);
  return outcome;
}

}  // namespace tournament_arena
