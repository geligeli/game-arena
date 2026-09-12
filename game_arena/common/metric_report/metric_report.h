#ifndef GAME_ARENA_GAME_ARENA_COMMON_METRIC_REPORT_METRIC_REPORT_H
#define GAME_ARENA_GAME_ARENA_COMMON_METRIC_REPORT_METRIC_REPORT_H

// Reading what a graded command measured.
//
// The contract is a JSON file at the path the runner passes in $ARENA_REPORT:
//
//   {"metrics": {"wall_ms": 1234.5, "peak_rss_mb": 91.2}}
//
// JSON so a command can emit it with `printf` and no library. A
// "RESULT wall_ms=1234.5" line on stdout is accepted as a fallback, matching
// the shape the match harness already prints, so a benchmark that only knows
// how to print a line is not shut out.
//
// Here rather than beside the thing that runs the command, because both ends
// of this format live in one repo on purpose: game_arena/grader writes it and
// the worker reads it back, and a test that holds the two together should not
// have to reach into the sandbox to do it.
//
// What to do with several runs' numbers is a separate question, and an
// arena-shaped one -- see sandbox/worker/grade_policy.h.

#include <map>
#include <string>
#include <string_view>

namespace metric_report {

// Parses one run's report, preferring |json| and falling back to a RESULT
// line in |stdout_text|. Returns false when neither form is present.
auto Parse(std::string_view json, std::string_view stdout_text,
           std::map<std::string, double> *metrics) -> bool;

}  // namespace metric_report

#endif  // GAME_ARENA_GAME_ARENA_COMMON_METRIC_REPORT_METRIC_REPORT_H
