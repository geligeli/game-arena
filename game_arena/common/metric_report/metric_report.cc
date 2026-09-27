#include "game_arena/common/metric_report/metric_report.h"

#include <google/protobuf/util/json_util.h>

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <map>
#include <string>
#include <string_view>

#include "absl/strings/str_split.h"
#include "absl/strings/strip.h"
#include "game_arena/proto/problem.pb.h"

namespace metric_report {

namespace {

// "RESULT a=1 b=2.5" -> {a: 1, b: 2.5}; the last RESULT line wins.
bool ParseResultLineMetrics(std::string_view text,
                            std::map<std::string, double> *metrics) {
  bool found = false;
  for (std::string_view line : absl::StrSplit(text, '\n')) {
    if (!absl::ConsumePrefix(&line, "RESULT ")) {
      continue;
    }
    std::map<std::string, double> parsed;
    // A key runs from the first non-space to the next '=', a value to the next
    // space; from_chars takes a numeric prefix ("1.5abc" reads as 1.5).
    for (;;) {
      line.remove_prefix(std::min(line.find_first_not_of(' '), line.size()));
      const std::size_t eq = line.find('=');
      if (eq == std::string_view::npos) {
        break;
      }
      const std::size_t end = std::min(line.find(' ', eq), line.size());
      double number = 0.0;
      if (eq > 0 &&
          std::from_chars(line.data() + eq + 1, line.data() + end, number).ec ==
              std::errc()) {
        parsed[std::string(line.substr(0, eq))] = number;
      }
      line.remove_prefix(end);
    }
    if (!parsed.empty()) {
      *metrics = std::move(parsed);
      found = true;
    }
  }
  return found;
}

}  // namespace

bool Parse(std::string_view json, std::string_view stdout_text,
           std::map<std::string, double> *metrics) {
  metrics->clear();
  if (!json.empty()) {
    tournament_arena::proto::MetricReport report;
    // A command reporting more than the schema knows is being helpful.
    google::protobuf::json::ParseOptions options;
    options.ignore_unknown_fields = true;
    if (google::protobuf::json::JsonStringToMessage(
            absl::string_view(json.data(), json.size()), &report, options)
            .ok()) {
      for (const auto &[name, value] : report.metrics()) {
        (*metrics)[name] = value;
      }
      if (!metrics->empty()) {
        return true;
      }
    }
  }
  return ParseResultLineMetrics(stdout_text, metrics);
}

}  // namespace metric_report
