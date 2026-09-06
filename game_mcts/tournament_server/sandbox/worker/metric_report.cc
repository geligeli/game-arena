#include "game_mcts/tournament_server/sandbox/worker/metric_report.h"

#include <google/protobuf/util/json_util.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstddef>
#include <map>
#include <numeric>
#include <string>
#include <string_view>
#include <vector>

#include "game_mcts/tournament_server/proto/problem.pb.h"

namespace tournament_arena {

namespace {

// "RESULT a=1 b=2.5" -> {a: 1, b: 2.5}. The last RESULT line wins, so a
// command that prints progress lines before its final one is fine.
auto ParseResultLineMetrics(std::string_view text,
                            std::map<std::string, double> *metrics) -> bool {
  constexpr std::string_view kMarker = "RESULT ";
  std::size_t line_start = 0;
  bool found = false;
  while (line_start <= text.size()) {
    const std::size_t nl = text.find('\n', line_start);
    const std::string_view line = text.substr(
        line_start, nl == std::string_view::npos ? std::string_view::npos
                                                 : nl - line_start);
    if (line.rfind(kMarker, 0) == 0) {
      std::map<std::string, double> parsed;
      std::size_t at = kMarker.size();
      while (at < line.size()) {
        while (at < line.size() && line[at] == ' ') {
          ++at;
        }
        const std::size_t eq = line.find('=', at);
        if (eq == std::string_view::npos) {
          break;
        }
        std::size_t end = line.find(' ', eq);
        if (end == std::string_view::npos) {
          end = line.size();
        }
        const std::string_view key = line.substr(at, eq - at);
        const std::string_view value = line.substr(eq + 1, end - eq - 1);
        double number = 0.0;
        const auto [ptr, ec] =
            std::from_chars(value.data(), value.data() + value.size(), number);
        if (ec == std::errc() && !key.empty()) {
          parsed[std::string(key)] = number;
        }
        at = end;
      }
      if (!parsed.empty()) {
        *metrics = std::move(parsed);
        found = true;
      }
    }
    if (nl == std::string_view::npos) {
      break;
    }
    line_start = nl + 1;
  }
  return found;
}

}  // namespace

auto ParseMetricReport(std::string_view json, std::string_view stdout_text,
                       std::map<std::string, double> *metrics) -> bool {
  metrics->clear();
  if (!json.empty()) {
    proto::MetricReport report;
    // Unknown fields are tolerated: a command that reports more than the schema
    // knows about is being helpful, not wrong.
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

auto AggregateMetrics(const std::vector<std::map<std::string, double>> &runs,
                      proto::GradeOrder::Aggregate how)
    -> std::map<std::string, double> {
  std::map<std::string, std::vector<double>> gathered;
  for (const auto &run : runs) {
    for (const auto &[name, value] : run) {
      gathered[name].push_back(value);
    }
  }

  std::map<std::string, double> out;
  for (auto &[name, values] : gathered) {
    if (values.empty()) {
      continue;
    }
    switch (how) {
      case proto::GradeOrder::MIN:
        out[name] = *std::min_element(values.begin(), values.end());
        break;
      case proto::GradeOrder::MEAN:
        out[name] = std::accumulate(values.begin(), values.end(), 0.0) /
                    static_cast<double>(values.size());
        break;
      case proto::GradeOrder::MEDIAN:
      default: {
        std::sort(values.begin(), values.end());
        const std::size_t mid = values.size() / 2;
        // An even number of runs takes the mean of the middle two, so the
        // median of two runs is their average rather than an arbitrary one.
        out[name] = values.size() % 2 == 1
                        ? values[mid]
                        : (values[mid - 1] + values[mid]) / 2.0;
        break;
      }
    }
  }
  return out;
}

}  // namespace tournament_arena
