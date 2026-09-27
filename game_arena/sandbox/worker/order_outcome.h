#ifndef GAME_ARENA_GAME_ARENA_SANDBOX_WORKER_ORDER_OUTCOME_H
#define GAME_ARENA_GAME_ARENA_SANDBOX_WORKER_ORDER_OUTCOME_H

// Reading a sandbox job's result as an order's outcome.
//
// The mirror of order_job.h, and the other half of what the two backends each
// had their own copy of: which step's log is the build log, whose fault a
// build failure was, that the referee's report carries the games and nothing
// a side printed does, and that a graded run's numbers are aggregated before
// they mean anything.
//
// One distinction runs through all of it: a step that ran and failed is the
// order's result, while a job the engine could not run at all is an error. A
// build that does not compile is a completed order with build_ok false, and
// the submitter needs the diagnostics; a missing docker binary is not the
// submitter's problem.

#include <vector>

#include "game_arena/proto/arena.pb.h"
#include "game_arena/proto/tournament_broker.pb.h"
#include "game_arena/sandbox/exec/sandbox_job.pb.h"

namespace tournament_arena {

struct OrderOutcome {
  proto::OrderResult result;  // order_id, worker_id and machine_class unset
  std::vector<tournament_broker::proto::GameRecord> games;
};

// Interprets |result| as the outcome of |order|.
OrderOutcome OutcomeFor(const proto::WorkOrder &order,
                        const sandbox_exec::proto::JobResult &result);

}  // namespace tournament_arena

#endif  // GAME_ARENA_GAME_ARENA_SANDBOX_WORKER_ORDER_OUTCOME_H
