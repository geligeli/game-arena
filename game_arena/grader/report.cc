#include "game_arena/grader/report.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>
#include <string>

namespace grader {
namespace {

// Enough digits to round-trip a double, without the exponent noise that a
// default ostream would produce for large scores.
auto FormatNumber(double value) -> std::string {
  char buffer[64];
  std::snprintf(buffer, sizeof(buffer), "%.17g", value);
  return buffer;
}

auto JsonEscape(const std::string &text) -> std::string {
  std::string out;
  out.reserve(text.size());
  for (const char c : text) {
    switch (c) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        out += c;
    }
  }
  return out;
}

}  // namespace

auto RenderReport(const std::map<std::string, double> &metrics) -> std::string {
  std::ostringstream out;
  out << "{\"metrics\": {";
  bool first = true;
  for (const auto &[name, value] : metrics) {
    if (!first) {
      out << ", ";
    }
    first = false;
    out << '"' << JsonEscape(name) << "\": " << FormatNumber(value);
  }
  out << "}}\n";
  return out.str();
}

auto WriteReport(const std::string &path,
                 const std::map<std::string, double> &metrics,
                 std::string *error) -> bool {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) {
    *error = "cannot open the report path '" + path + "' for writing";
    return false;
  }
  out << RenderReport(metrics);
  out.close();
  if (!out) {
    *error = "failed while writing the report to '" + path + "'";
    return false;
  }
  return true;
}

auto WriteReportToArenaPath(const std::map<std::string, double> &metrics,
                            std::string *error) -> bool {
  const char *path = std::getenv("ARENA_REPORT");
  if (path == nullptr || *path == '\0') {
    *error =
        "$ARENA_REPORT is not set: this command is not running as a graded "
        "order under the arena";
    return false;
  }
  return WriteReport(path, metrics, error);
}

}  // namespace grader
