#ifndef GAME_ARENA_GAME_ARENA_SANDBOX_WORKER_ORDER_OUTCOME_H
#define GAME_ARENA_GAME_ARENA_SANDBOX_WORKER_ORDER_OUTCOME_H

// A job's result as an order's: a step that failed is a result, a job the
// engine could not run is an error.

#include <map>
#include <string>
#include <vector>

#include "game_arena/proto/arena.pb.h"
#include "game_arena/proto/tournament_broker.pb.h"
#include "game_arena/sandbox/exec/sandbox_job.pb.h"

namespace tournament_arena {

struct OrderOutcome {
  proto::OrderResult result;  // order_id, worker_id and machine_class unset
  std::vector<tournament_broker::proto::GameRecord> games;
  // A build_only order's binaries: build target -> bytes, to archive.
  std::map<std::string, std::string> built;
};

// |prebuilt|: the job had no build phase, as it ran the archive's binaries.
OrderOutcome OutcomeFor(const proto::WorkOrder &order,
                        const sandbox_exec::proto::JobResult &result,
                        bool prebuilt = false);

}  // namespace tournament_arena

#endif  // GAME_ARENA_GAME_ARENA_SANDBOX_WORKER_ORDER_OUTCOME_H
