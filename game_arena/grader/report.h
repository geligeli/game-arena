#ifndef GAME_ARENA_GAME_ARENA_GRADER_REPORT_H
#define GAME_ARENA_GAME_ARENA_GRADER_REPORT_H

// Writing the file a graded command is scored from.
//
// The contract is a JSON object at the path in $ARENA_REPORT:
//
//   {"metrics": {"score": 87, "solved": 42}}
//
// which the worker reads back (common/metric_report/metric_report.h). It is
// simple enough to emit with printf, and a grader in another language should.
// This exists so a grader that is already C++ does not hand-roll it, and so the
// writer and the reader are in one repo and cannot drift -- report_test round
// trips through the real parser.
//
// A metric the problem did not declare is not an error here. The coordinator
// keeps what it was asked to rank on and ignores the rest, so a grader is free
// to report extra numbers that are only useful when something looks wrong.

#include <map>
#include <string>

namespace grader {

// Writes the report to |path|. Returns false (with *error set) if it cannot be
// written -- the caller should exit non-zero, because a graded run that reports
// nothing is not a zero score, it is a failed run.
auto WriteReport(const std::string &path,
                 const std::map<std::string, double> &metrics,
                 std::string *error) -> bool;

// The same, to the path in $ARENA_REPORT. Returns false if the variable is
// unset, which means the command is not running under the arena.
auto WriteReportToArenaPath(const std::map<std::string, double> &metrics,
                            std::string *error) -> bool;

// The JSON body, exposed for tests and for graders that want to place it
// themselves.
auto RenderReport(const std::map<std::string, double> &metrics) -> std::string;

}  // namespace grader

#endif  // GAME_ARENA_GAME_ARENA_GRADER_REPORT_H
