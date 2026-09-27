#ifndef GAME_ARENA_GAME_ARENA_SANDBOX_WORKER_GRADE_POLICY_H
#define GAME_ARENA_GAME_ARENA_SANDBOX_WORKER_GRADE_POLICY_H

#include <map>
#include <string>
#include <vector>

#include "game_arena/proto/arena.pb.h"

namespace tournament_arena {

// A metric missing from some runs is folded over the runs that have it.
std::map<std::string, double> AggregateMetrics(
    const std::vector<std::map<std::string, double>> &runs,
    proto::GradeOrder::Aggregate how);

// Only the metrics the problem ranks on, or a report could grow the standings
// file without bound.
std::map<std::string, double> ScoreGradedRuns(
    const std::vector<std::map<std::string, double>> &runs,
    const proto::GradeOrder &grade);

}  // namespace tournament_arena

#endif  // GAME_ARENA_GAME_ARENA_SANDBOX_WORKER_GRADE_POLICY_H
