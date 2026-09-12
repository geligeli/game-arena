#ifndef GAME_ARENA_GAME_ARENA_SANDBOX_WORKER_ORDER_OUTCOME_H
#define GAME_ARENA_GAME_ARENA_SANDBOX_WORKER_ORDER_OUTCOME_H

// Reading a sandbox job's result as an order's outcome.
//
// The mirror of order_job.h, and the other half of what the two backends each
// had their own copy of: which step's log is the build log, whose fault a
// build failure was, that the referee's stdout carries the tally and the
// bot's does not, and that a graded run's numbers are aggregated before they
// mean anything.
//
// One distinction runs through all of it: a step that ran and failed is the
// order's result, while a job the engine could not run at all is an error. A
// build that does not compile is a completed order with build_ok false, and
// the submitter needs the diagnostics; a missing docker binary is not the
// submitter's problem.

#include <map>
#include <string>
#include <vector>

#include "game_arena/proto/arena.pb.h"
#include "game_arena/sandbox/exec/sandbox_job.pb.h"

namespace tournament_arena {

struct OrderOutcome {
  bool build_ok = false;
  // Already compacted: full bazel logs never leave the worker.
  std::string build_log;
  // Whose build broke, when build_ok is false. An order builds both sides of
  // a match, and the opponent failing to compile is not the submitter's fault
  // -- without this the coordinator would retire the wrong submission. Empty
  // means the order's own candidate.
  std::string build_failed_candidate_id;
  // Games for a match order, measurement runs for a graded one.
  int games_played = 0;
  int wins = 0;
  int draws = 0;
  int losses = 0;
  double elo = 0.0;
  // What a graded order measured, already aggregated across runs and filtered
  // to the metrics the problem ranks on. Empty for a match order.
  std::map<std::string, double> metrics;
  // Non-empty when the order could not be completed at all -- checkout
  // failed, the bot crashed, a step timed out. Distinct from a clean build
  // that simply lost every game.
  std::string error;
};

// Interprets |result| as the outcome of |order|.
auto OutcomeFor(const proto::WorkOrder &order,
                const sandbox_exec::proto::JobResult &result) -> OrderOutcome;

}  // namespace tournament_arena

#endif  // GAME_ARENA_GAME_ARENA_SANDBOX_WORKER_ORDER_OUTCOME_H
