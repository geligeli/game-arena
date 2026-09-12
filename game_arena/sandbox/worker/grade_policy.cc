#include "game_arena/sandbox/worker/grade_policy.h"

#include <algorithm>
#include <map>
#include <numeric>
#include <string>
#include <vector>

namespace tournament_arena {

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

auto ScoreGradedRuns(const std::vector<std::map<std::string, double>> &runs,
                     const proto::GradeOrder &grade)
    -> std::map<std::string, double> {
  const std::map<std::string, double> aggregated =
      AggregateMetrics(runs, grade.aggregate());
  std::map<std::string, double> scored;
  for (const std::string &name : grade.metric_names()) {
    const auto it = aggregated.find(name);
    if (it != aggregated.end()) {
      scored[name] = it->second;
    }
  }
  return scored;
}

}  // namespace tournament_arena
