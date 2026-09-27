#ifndef GAME_ARENA_GAME_ARENA_COMMON_METRIC_REPORT_METRIC_REPORT_H
#define GAME_ARENA_GAME_ARENA_COMMON_METRIC_REPORT_METRIC_REPORT_H

// What a graded command measured: {"metrics": {"wall_ms": 1234.5}} at
// $ARENA_REPORT, or failing that a "RESULT wall_ms=1234.5" line on stdout.

#include <map>
#include <string>
#include <string_view>

namespace metric_report {

// False when neither form is present.
bool Parse(std::string_view json, std::string_view stdout_text,
           std::map<std::string, double> *metrics);

}  // namespace metric_report

#endif  // GAME_ARENA_GAME_ARENA_COMMON_METRIC_REPORT_METRIC_REPORT_H
