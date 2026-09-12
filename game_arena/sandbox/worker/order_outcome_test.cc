// Reading a job's result as an order's outcome.
//
// The distinction under test throughout: a step that ran and failed is the
// order's result, and a job the engine could not run is an error. Getting
// that backwards either hides a compiler diagnostic from the submitter or
// retires a submission for the fleet's fault.

#include "game_arena/sandbox/worker/order_outcome.h"

#include <string>

#include "gtest/gtest.h"

namespace tournament_arena {
namespace {

namespace sx = sandbox_exec::proto;

auto MatchOrder(int games = 2) -> proto::WorkOrder {
  proto::WorkOrder order;
  order.set_num_games(games);
  order.mutable_candidate()->set_candidate_id("c-ok");
  return order;
}

auto GradedOrder(const std::string &metric = "wall_ms") -> proto::WorkOrder {
  proto::WorkOrder order;
  order.mutable_candidate()->set_candidate_id("c-ok");
  proto::GradeOrder *grade = order.mutable_grade();
  grade->add_argv("./bench");
  grade->add_metric_names(metric);
  grade->set_aggregate(proto::GradeOrder::MIN);
  return order;
}

auto AddStep(sx::PhaseResult *phase, const std::string &name, int exit_code,
             const std::string &out = "") -> sx::StepResult * {
  sx::StepResult *step = phase->add_steps();
  step->set_name(name);
  step->set_started(true);
  step->set_exit_code(exit_code);
  step->set_stdout(out);
  return step;
}

auto WithBuild(sx::JobResult *result, int exit_code,
               const std::string &out = "") -> sx::StepResult * {
  sx::PhaseResult *phase = result->add_phases();
  phase->set_name("build");
  return AddStep(phase, "build", exit_code, out);
}

TEST(OutcomeForTest, AGoodMatchCarriesTheRefereesTally) {
  sx::JobResult result;
  WithBuild(&result, 0);
  sx::PhaseResult *match = result.add_phases();
  match->set_name("match");
  AddStep(match, "bot", 0);
  // The referee's tally, not the bot's: the bot only knows what it was told.
  AddStep(match, "referee", 0,
          "RESULT games=2 wins=1 draws=1 losses=0 elo=1512.5\n");

  const OrderOutcome outcome = OutcomeFor(MatchOrder(), result);
  EXPECT_TRUE(outcome.build_ok);
  EXPECT_EQ(outcome.error, "");
  EXPECT_EQ(outcome.games_played, 2);
  EXPECT_EQ(outcome.wins, 1);
  EXPECT_EQ(outcome.draws, 1);
  EXPECT_EQ(outcome.losses, 0);
  EXPECT_DOUBLE_EQ(outcome.elo, 1512.5);
}

TEST(OutcomeForTest, ABuildFailureIsACompletedOrderWithDiagnostics) {
  sx::JobResult result;
  WithBuild(&result, 1,
            "solutions/c-ok/strategy.h:3:5: error: expected ';'\n"
            "Target //solutions/c-ok:bot failed to build\n");

  const OrderOutcome outcome = OutcomeFor(MatchOrder(), result);
  // Not an error: the submitter needs the compiler output, and the order did
  // complete.
  EXPECT_FALSE(outcome.build_ok);
  EXPECT_EQ(outcome.error, "");
  EXPECT_NE(outcome.build_log.find("expected ';'"), std::string::npos)
      << outcome.build_log;
}

TEST(OutcomeForTest, AnOpponentsBuildFailureIsNotTheSubmittersFault) {
  proto::WorkOrder order = MatchOrder();
  order.mutable_opponent()->set_candidate_id("c-rival");

  sx::JobResult result;
  WithBuild(&result, 1,
            "solutions/c-rival/strategy.h:9:1: error: no member named 'x'\n");

  // Without this the coordinator would retire the wrong submission.
  EXPECT_EQ(OutcomeFor(order, result).build_failed_candidate_id, "c-rival");
}

TEST(OutcomeForTest, ABuildTimeoutNamesTheTimeoutThatActuallyApplied) {
  sx::JobResult result;
  sx::StepResult *build = WithBuild(&result, 0, "compiling...\n");
  build->set_timed_out(true);
  build->set_timeout_s(1800);

  // The effective timeout, not the requested one. An order that left
  // build_timeout_s unset used to report "build timed out after 0s" after
  // half an hour.
  EXPECT_EQ(OutcomeFor(MatchOrder(), result).error,
            "build timed out after 1800s");
}

TEST(OutcomeForTest, AJobTheEngineCouldNotRunIsAnErrorNotAResult) {
  sx::JobResult result;
  result.mutable_status()->set_code(sx::Status::WORKSPACE_FAILED);
  result.mutable_status()->set_message("cannot mount the workspace overlay");

  const OrderOutcome outcome = OutcomeFor(MatchOrder(), result);
  EXPECT_FALSE(outcome.build_ok);
  EXPECT_EQ(outcome.error, "cannot mount the workspace overlay");
  EXPECT_EQ(outcome.games_played, 0);
}

TEST(OutcomeForTest, ACancelledJobSaysSoRatherThanLookingLikeAFailure) {
  sx::JobResult result;
  result.mutable_status()->set_code(sx::Status::CANCELLED);
  result.mutable_status()->set_message("cancelled");

  EXPECT_EQ(OutcomeFor(MatchOrder(), result).error, "cancelled");
}

TEST(OutcomeForTest, ARefereeThatSaidNothingIsAnErrorWithItsOutput) {
  sx::JobResult result;
  WithBuild(&result, 0);
  sx::PhaseResult *match = result.add_phases();
  match->set_name("match");
  AddStep(match, "bot", 0);
  sx::StepResult *referee = AddStep(match, "referee", 1);
  referee->set_stderr("could not bind\n");

  const OrderOutcome outcome = OutcomeFor(MatchOrder(), result);
  EXPECT_NE(outcome.error.find("referee produced no result"), std::string::npos)
      << outcome.error;
  EXPECT_NE(outcome.error.find("could not bind"), std::string::npos);
}

TEST(OutcomeForTest, ABotThatTimedOutNamesTheTimeout) {
  sx::JobResult result;
  WithBuild(&result, 0);
  sx::PhaseResult *match = result.add_phases();
  match->set_name("match");
  sx::StepResult *bot = AddStep(match, "bot", 124);
  bot->set_timed_out(true);
  bot->set_timeout_s(2);
  AddStep(match, "referee", 0);

  // The two backends disagreed here: one named the timeout, the other
  // reported a missing referee result, which describes a symptom.
  EXPECT_EQ(OutcomeFor(MatchOrder(), result).error, "games timed out after 2s");
}

TEST(OutcomeForTest, AShortMatchIsRecordedNotDiscarded) {
  sx::JobResult result;
  WithBuild(&result, 0);
  sx::PhaseResult *match = result.add_phases();
  match->set_name("match");
  AddStep(match, "bot", 0);
  AddStep(match, "referee", 0,
          "RESULT games=1 wins=1 draws=0 losses=0 elo=1505.0\n");

  // The games that were played are real results: an agent is better served by
  // a short match plus the reason than by nothing.
  const OrderOutcome outcome = OutcomeFor(MatchOrder(4), result);
  EXPECT_EQ(outcome.games_played, 1);
  EXPECT_EQ(outcome.wins, 1);
  EXPECT_NE(outcome.error.find("match was short: 1 of 4"), std::string::npos)
      << outcome.error;
}

TEST(OutcomeForTest, AGradedOrderAggregatesEveryRunsReport) {
  sx::JobResult result;
  WithBuild(&result, 0);
  for (const char *value : {"120", "100", "140"}) {
    sx::PhaseResult *phase = result.add_phases();
    phase->set_name("grade");
    sx::StepResult *step = AddStep(phase, "grade", 0);
    (*step->mutable_collected())["report.json"] =
        std::string("{\"metrics\": {\"wall_ms\": ") + value + "}}";
  }

  const OrderOutcome outcome = OutcomeFor(GradedOrder(), result);
  EXPECT_EQ(outcome.error, "");
  ASSERT_TRUE(outcome.metrics.contains("wall_ms"));
  EXPECT_DOUBLE_EQ(outcome.metrics.at("wall_ms"), 100.0);
  // Runs, for a graded order: the same field a match fills with games.
  EXPECT_EQ(outcome.games_played, 3);
}

TEST(OutcomeForTest, ANonzeroExitIsNeverScoredWhateverItPrinted) {
  sx::JobResult result;
  WithBuild(&result, 0);
  sx::PhaseResult *phase = result.add_phases();
  phase->set_name("grade");
  sx::StepResult *step = AddStep(phase, "grade", 3);
  (*step->mutable_collected())["report.json"] =
      "{\"metrics\": {\"wall_ms\": 1}}";

  // A number from a failed run looks like a result, which is worse than no
  // number at all.
  const OrderOutcome outcome = OutcomeFor(GradedOrder(), result);
  EXPECT_TRUE(outcome.metrics.empty());
  EXPECT_NE(outcome.error.find("exited 3"), std::string::npos) << outcome.error;
}

TEST(OutcomeForTest, MetricsTheProblemDoesNotRankOnAreDropped) {
  sx::JobResult result;
  WithBuild(&result, 0);
  sx::PhaseResult *phase = result.add_phases();
  phase->set_name("grade");
  sx::StepResult *step = AddStep(phase, "grade", 0);
  (*step->mutable_collected())["report.json"] =
      "{\"metrics\": {\"wall_ms\": 12, \"peak_rss_mb\": 40}}";

  const OrderOutcome outcome = OutcomeFor(GradedOrder(), result);
  EXPECT_TRUE(outcome.metrics.contains("wall_ms"));
  EXPECT_FALSE(outcome.metrics.contains("peak_rss_mb"));
}

TEST(OutcomeForTest, SaysSoWhenNoneOfTheProblemsMetricsWereReported) {
  sx::JobResult result;
  WithBuild(&result, 0);
  sx::PhaseResult *phase = result.add_phases();
  phase->set_name("grade");
  sx::StepResult *step = AddStep(phase, "grade", 0);
  (*step->mutable_collected())["report.json"] =
      "{\"metrics\": {\"something_else\": 1}}";

  EXPECT_NE(OutcomeFor(GradedOrder(), result)
                .error.find("none of this problem's metrics"),
            std::string::npos);
}

TEST(OutcomeForTest, AResultLineIsAcceptedInsteadOfAReport) {
  sx::JobResult result;
  WithBuild(&result, 0);
  sx::PhaseResult *phase = result.add_phases();
  phase->set_name("grade");
  AddStep(phase, "grade", 0, "RESULT wall_ms=37.5\n");

  // So a benchmark that only knows how to print a line is not shut out.
  const OrderOutcome outcome = OutcomeFor(GradedOrder(), result);
  ASSERT_TRUE(outcome.metrics.contains("wall_ms"));
  EXPECT_DOUBLE_EQ(outcome.metrics.at("wall_ms"), 37.5);
}

}  // namespace
}  // namespace tournament_arena
