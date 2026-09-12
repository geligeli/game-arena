#ifndef GAME_ARENA_GAME_ARENA_SANDBOX_WORKER_GRADE_POLICY_H
#define GAME_ARENA_GAME_ARENA_SANDBOX_WORKER_GRADE_POLICY_H

// Turning several runs' measurements into the numbers a leaderboard stores.
//
// Aggregating is the point of running more than once: a single timing is
// noise. Which way to fold the runs, and which metrics count at all, are the
// problem's decisions -- which is why this is arena policy and lives here,
// while the report format itself is neutral
// (game_arena/common/metric_report).

#include <map>
#include <string>
#include <vector>

#include "game_arena/proto/arena.pb.h"

namespace tournament_arena {

// Folds |runs| into one value per metric. A metric missing from some runs is
// aggregated over the runs that have it: a benchmark that only reports peak
// RSS on some platforms should still contribute what it measured.
auto AggregateMetrics(const std::vector<std::map<std::string, double>> &runs,
                      proto::GradeOrder::Aggregate how)
    -> std::map<std::string, double>;

// Aggregates |runs| per |grade|, then keeps only the metrics the problem ranks
// on. A benchmark printing more than that is normal; storing it all would let
// a report grow the standings file without bound.
auto ScoreGradedRuns(const std::vector<std::map<std::string, double>> &runs,
                     const proto::GradeOrder &grade)
    -> std::map<std::string, double>;

}  // namespace tournament_arena

#endif  // GAME_ARENA_GAME_ARENA_SANDBOX_WORKER_GRADE_POLICY_H
