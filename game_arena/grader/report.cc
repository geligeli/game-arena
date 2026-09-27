#include "game_arena/grader/report.h"

#include <boost/json/object.hpp>
#include <boost/json/serialize.hpp>
#include <cstdlib>
#include <fstream>
#include <map>
#include <string>

namespace grader {

std::string RenderReport(const std::map<std::string, double> &metrics) {
  boost::json::object values;
  for (const auto &[name, value] : metrics) values[name] = value;
  return boost::json::serialize(boost::json::object{{"metrics", values}}) +
         "\n";
}

bool WriteReportToArenaPath(const std::map<std::string, double> &metrics,
                            std::string *error) {
  const char *path = std::getenv("ARENA_REPORT");
  if (path == nullptr || *path == '\0') {
    *error =
        "$ARENA_REPORT is not set: this command is not running as a graded "
        "order under the arena";
    return false;
  }
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) {
    *error =
        "cannot open the report path '" + std::string(path) + "' for writing";
    return false;
  }
  out << RenderReport(metrics);
  out.close();
  if (!out) {
    *error = "failed while writing the report to '" + std::string(path) + "'";
    return false;
  }
  return true;
}

}  // namespace grader
