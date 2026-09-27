#include "game_arena/sandbox/worker/order_outcome.h"

#include <map>
#include <string>
#include <utility>
#include <vector>

#include "game_arena/common/metric_report/metric_report.h"
#include "game_arena/referee/match_tally.h"
#include "game_arena/sandbox/common/text.h"
#include "game_arena/sandbox/worker/build_log.h"
#include "game_arena/sandbox/worker/grade_policy.h"
#include "game_arena/sandbox/worker/order_job.h"

namespace tournament_arena {

namespace sx = sandbox_exec::proto;

namespace {

using sandbox_common::TailOf;

const sx::StepResult *StepNamed(const sx::PhaseResult &phase,
                                const std::string &name) {
  for (const sx::StepResult &step : phase.steps()) {
    if (step.name() == name) {
      return &step;
    }
  }
  return nullptr;
}

const sx::PhaseResult *PhaseNamed(const sx::JobResult &result,
                                  const std::string &name) {
  for (const sx::PhaseResult &phase : result.phases()) {
    if (phase.name() == name) {
      return &phase;
    }
  }
  return nullptr;
}

// The order's own candidate is the one being evaluated; the opponent's build
// breaking is somebody else's problem and must not retire this submission.
std::string BlameForBuild(const proto::WorkOrder &order,
                          const std::string &log) {
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

void ReadMatch(const proto::WorkOrder &order, const sx::PhaseResult &match,
               OrderOutcome *outcome) {
  const sx::StepResult *referee = StepNamed(match, "referee");
  const sx::StepResult *bot = StepNamed(match, "bot");
  const std::string referee_output =
      referee != nullptr ? referee->stdout() : "";
  const std::string referee_errors =
      referee != nullptr ? referee->stderr() : "";

  // From the referee's report, not from anything a side printed: the referee
  // applied every move, and its private scratch is out of either side's reach.
  // An empty report is no report: the engine collects nothing for an empty
  // file, and a match with no games has nothing to say either.
  tournament_broker::proto::MatchReport report;
  if (referee == nullptr || referee->collected().count(kMatchReport) == 0 ||
      !report.ParseFromString(referee->collected().at(kMatchReport))) {
    outcome->result.set_error("referee produced no result" +
                              std::string(bot != nullptr && bot->timed_out()
                                              ? " (the bot timed out first)"
                                              : "") +
                              ": " +
                              TailOf(referee_errors + referee_output, 1500));
    return;
  }
  const tournament_broker::MatchTally tally =
      tournament_broker::TallyOf(report, order.candidate().candidate_id());
  if (tally.games < order.num_games()) {
    // Recorded, not fatal: the games that were played are real results, and
    // an agent is better served by a short match plus the reason than by
    // nothing.
    outcome->result.set_error(
        "match was short: " + std::to_string(tally.games) + " of " +
        std::to_string(order.num_games()) + " games played");
  }
  outcome->result.set_games_played(tally.games);
  outcome->result.set_wins(tally.wins);
  outcome->result.set_draws(tally.draws);
  outcome->result.set_losses(tally.losses);
  outcome->games.assign(report.games().begin(), report.games().end());
}

void ReadGrade(const proto::WorkOrder &order, const sx::JobResult &result,
               OrderOutcome *outcome) {
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
      outcome->result.set_error("graded run timed out after " +
                                std::to_string(step->timeout_s()) + "s");
      return;
    }
    if (step->exit_code() != 0) {
      // A number from a failed run looks like a result, which is worse than
      // no number at all.
      outcome->result.set_error("graded command exited " +
                                std::to_string(step->exit_code()) + ": " +
                                TailOf(step->stderr() + step->stdout(), 1000));
      return;
    }
    std::map<std::string, double> metrics;
    const auto collected = step->collected().find("report.json");
    const std::string report =
        collected != step->collected().end() ? collected->second : "";
    if (!metric_report::Parse(report, step->stdout(), &metrics)) {
      outcome->result.set_error(
          "graded run produced no metrics: write JSON to $ARENA_REPORT or "
          "print a RESULT line. Output was: " +
          TailOf(step->stdout(), 1000));
      return;
    }
    runs.push_back(std::move(metrics));
  }

  for (const auto &[name, value] : ScoreGradedRuns(runs, order.grade())) {
    (*outcome->result.mutable_metrics())[name] = value;
  }
  if (outcome->result.metrics().empty()) {
    outcome->result.set_error(
        "the graded command reported none of this problem's metrics");
    return;
  }
  // Runs, for a graded order: the same field a match fills with games.
  outcome->result.set_games_played(static_cast<int>(runs.size()));
}

}  // namespace

OrderOutcome OutcomeFor(const proto::WorkOrder &order,
                        const sx::JobResult &result) {
  OrderOutcome outcome;
  proto::OrderResult &out = outcome.result;

  const sx::PhaseResult *build = PhaseNamed(result, "build");
  const sx::StepResult *build_step =
      build != nullptr ? StepNamed(*build, "build") : nullptr;
  if (build_step != nullptr) {
    out.set_build_output(sandbox_common::TailOf(
        build_step->stdout() + build_step->stderr(), 64 << 10));
  }

  // Whatever the engine says it could not do, before looking at any step: a
  // job that never ran has no result to read.
  if (result.status().code() != sx::Status::OK) {
    if (build_step != nullptr && !build_step->stdout().empty()) {
      out.set_build_log(
          CompactBuildLog(build_step->stdout() + build_step->stderr()));
    }
    out.set_error(result.status().message());
    return outcome;
  }

  if (build_step == nullptr) {
    out.set_error("the job reported no build");
    return outcome;
  }
  const std::string build_output = build_step->stdout() + build_step->stderr();
  if (build_step->timed_out()) {
    out.set_build_log(CompactBuildLog(build_output));
    // The timeout the engine actually enforced, not the one the order asked
    // for: an order that leaves build_timeout_s unset used to report "build
    // timed out after 0s".
    out.set_error("build timed out after " +
                  std::to_string(build_step->timeout_s()) + "s");
    return outcome;
  }
  if (build_step->exit_code() != 0) {
    // A build failure is the candidate's fault, not the order's: report it as
    // a completed order with build_ok false so the agent gets the
    // diagnostics.
    out.set_build_log(CompactBuildLog(build_output));
    out.set_build_failed_candidate_id(BlameForBuild(order, build_output));
    return outcome;
  }
  out.set_build_ok(true);

  if (order.has_grade()) {
    ReadGrade(order, result, &outcome);
    return outcome;
  }

  const sx::PhaseResult *match = PhaseNamed(result, "match");
  if (match == nullptr) {
    out.set_error("the job reported no match");
    return outcome;
  }
  const sx::StepResult *bot = StepNamed(*match, "bot");
  if (bot != nullptr && !bot->started()) {
    out.set_error("the bot could not be started");
    return outcome;
  }
  if (bot != nullptr && bot->timed_out()) {
    // Named rather than reported as a missing referee result: the two
    // backends disagreed about this, and "games timed out after 2s" says what
    // happened while "referee produced no result" describes a symptom.
    out.set_error("games timed out after " + std::to_string(bot->timeout_s()) +
                  "s");
    return outcome;
  }
  ReadMatch(order, *match, &outcome);
  return outcome;
}

}  // namespace tournament_arena
