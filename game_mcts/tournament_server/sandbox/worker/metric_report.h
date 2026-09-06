#ifndef GAME_MCTS_GAME_MCTS_TOURNAMENT_SERVER_SANDBOX_WORKER_METRIC_REPORT_H
#define GAME_MCTS_GAME_MCTS_TOURNAMENT_SERVER_SANDBOX_WORKER_METRIC_REPORT_H

// Reading what a graded command measured, and folding several runs into one
// number per metric.
//
// The contract is a JSON file at the path the worker passes in $ARENA_REPORT:
//
//   {"metrics": {"wall_ms": 1234.5, "peak_rss_mb": 91.2}}
//
// JSON so a command can emit it with `printf` and no library. A
// "RESULT wall_ms=1234.5" line on stdout is accepted as a fallback, matching
// the shape the match harness already prints, so a benchmark that only knows
// how to print a line is not shut out.
//
// Aggregating is the point of running more than once. A single timing is noise;
// which way to fold the runs is the problem's decision, not this file's.

#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "game_mcts/tournament_server/proto/arena.pb.h"

namespace tournament_arena {

// Parses one run's report. Returns false when neither form is present.
auto ParseMetricReport(std::string_view json, std::string_view stdout_text,
                       std::map<std::string, double> *metrics) -> bool;

// Folds |runs| into one value per metric. A metric missing from some runs is
// aggregated over the runs that have it: a benchmark that only reports peak RSS
// on some platforms should still contribute what it measured.
auto AggregateMetrics(const std::vector<std::map<std::string, double>> &runs,
                      proto::GradeOrder::Aggregate how)
    -> std::map<std::string, double>;

}  // namespace tournament_arena

#endif  // GAME_MCTS_GAME_MCTS_TOURNAMENT_SERVER_SANDBOX_WORKER_METRIC_REPORT_H
