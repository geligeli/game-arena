// Folding several runs into the number a leaderboard stores.

#include "game_arena/sandbox/worker/grade_policy.h"

#include <map>
#include <string>
#include <vector>

#include "gtest/gtest.h"

namespace tournament_arena {
namespace {

using MetricMap = std::map<std::string, double>;

TEST(AggregateMetricsTest, MinTakesTheBestRun) {
  const auto out = AggregateMetrics(
      {MetricMap{{"wall_ms", 300}}, MetricMap{{"wall_ms", 100}},
       MetricMap{{"wall_ms", 200}}},
      proto::GradeOrder::MIN);
  EXPECT_EQ(out.at("wall_ms"), 100.0);
}

TEST(AggregateMetricsTest, MeanAveragesThem) {
  const auto out = AggregateMetrics(
      {MetricMap{{"wall_ms", 100}}, MetricMap{{"wall_ms", 200}},
       MetricMap{{"wall_ms", 300}}},
      proto::GradeOrder::MEAN);
  EXPECT_EQ(out.at("wall_ms"), 200.0);
}

TEST(AggregateMetricsTest, MedianHandlesOddAndEvenRunCounts) {
  EXPECT_EQ(AggregateMetrics(
                {MetricMap{{"m", 5}}, MetricMap{{"m", 1}}, MetricMap{{"m", 3}}},
                proto::GradeOrder::MEDIAN)
                .at("m"),
            3.0);
  // An even count averages the middle two rather than picking one arbitrarily.
  EXPECT_EQ(AggregateMetrics({MetricMap{{"m", 10}}, MetricMap{{"m", 20}}},
                             proto::GradeOrder::MEDIAN)
                .at("m"),
            15.0);
}

// A benchmark that only reports peak RSS on some platforms should still
// contribute what it measured.
TEST(AggregateMetricsTest, AggregatesOverTheRunsThatHaveEachMetric) {
  const auto out = AggregateMetrics(
      {MetricMap{{"wall_ms", 100}, {"rss", 50}}, MetricMap{{"wall_ms", 200}}},
      proto::GradeOrder::MIN);
  EXPECT_EQ(out.at("wall_ms"), 100.0);
  EXPECT_EQ(out.at("rss"), 50.0);
}

TEST(AggregateMetricsTest, EmptyInputGivesEmptyOutput) {
  EXPECT_TRUE(AggregateMetrics({}, proto::GradeOrder::MIN).empty());
}

TEST(ScoreGradedRunsTest, KeepsOnlyWhatTheProblemRanksOn) {
  proto::GradeOrder grade;
  grade.set_aggregate(proto::GradeOrder::MIN);
  grade.add_metric_names("wall_ms");

  const std::map<std::string, double> scored =
      ScoreGradedRuns({{{"wall_ms", 120.0}, {"peak_rss_mb", 40.0}},
                       {{"wall_ms", 100.0}, {"peak_rss_mb", 44.0}}},
                      grade);

  EXPECT_EQ(scored.size(), 1u);
  EXPECT_DOUBLE_EQ(scored.at("wall_ms"), 100.0);
  EXPECT_EQ(scored.count("peak_rss_mb"), 0u);
}

TEST(ScoreGradedRunsTest, RanksOnNothingWhenTheMetricNeverAppeared) {
  proto::GradeOrder grade;
  grade.add_metric_names("wall_ms");
  EXPECT_TRUE(ScoreGradedRuns({{{"other", 1.0}}}, grade).empty());
}

}  // namespace
}  // namespace tournament_arena
