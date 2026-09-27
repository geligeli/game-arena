#ifndef GAME_ARENA_GAME_ARENA_GRADER_REPORT_H
#define GAME_ARENA_GAME_ARENA_GRADER_REPORT_H

// Writes {"metrics": {"score": 87, ...}} to $ARENA_REPORT, the file a graded
// command is scored from; common/metric_report reads it back.

#include <map>
#include <string>

namespace grader {

// Writes the report to $ARENA_REPORT. False, with *error set, when that is
// unset or unwritable. Exit non-zero then: a run that reports nothing failed.
bool WriteReportToArenaPath(const std::map<std::string, double> &metrics,
                            std::string *error);

// The JSON body, for tests and for graders that place it themselves.
std::string RenderReport(const std::map<std::string, double> &metrics);

}  // namespace grader

#endif  // GAME_ARENA_GAME_ARENA_GRADER_REPORT_H
