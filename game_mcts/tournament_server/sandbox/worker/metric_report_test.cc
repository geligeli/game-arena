#include "game_mcts/tournament_server/sandbox/worker/metric_report.h"

#include <gtest/gtest.h>

#include <map>
#include <string>
#include <vector>

namespace tournament_arena {
namespace {

using MetricMap = std::map<std::string, double>;

TEST(ParseMetricReportTest, ReadsTheJsonReport) {
  MetricMap metrics;
  ASSERT_TRUE(ParseMetricReport(
      R"({"metrics": {"wall_ms": 1234.5, "peak_rss_mb": 91.25}})", "",
      &metrics));
  EXPECT_EQ(metrics.at("wall_ms"), 1234.5);
  EXPECT_EQ(metrics.at("peak_rss_mb"), 91.25);
}

// A benchmark reporting more than the problem ranks on is normal; failing the
// run over it would be hostile.
TEST(ParseMetricReportTest, ToleratesUnknownJsonFields) {
  MetricMap metrics;
  ASSERT_TRUE(ParseMetricReport(
      R"({"metrics": {"wall_ms": 10}, "notes": "warm cache", "runs": 3})", "",
      &metrics));
  EXPECT_EQ(metrics.at("wall_ms"), 10.0);
}

// A benchmark that only knows how to print a line is not shut out.
TEST(ParseMetricReportTest, FallsBackToTheResultLine) {
  MetricMap metrics;
  ASSERT_TRUE(ParseMetricReport(
      "", "building...\nRESULT wall_ms=250.5 peak_rss_mb=64\ndone\n",
      &metrics));
  EXPECT_EQ(metrics.at("wall_ms"), 250.5);
  EXPECT_EQ(metrics.at("peak_rss_mb"), 64.0);
}

TEST(ParseMetricReportTest, TheLastResultLineWins) {
  MetricMap metrics;
  ASSERT_TRUE(ParseMetricReport("", "RESULT wall_ms=900\nRESULT wall_ms=100\n",
                                &metrics));
  EXPECT_EQ(metrics.at("wall_ms"), 100.0);
}

TEST(ParseMetricReportTest, PrefersTheJsonReportOverStdout) {
  MetricMap metrics;
  ASSERT_TRUE(ParseMetricReport(R"({"metrics": {"wall_ms": 1}})",
                                "RESULT wall_ms=999\n", &metrics));
  EXPECT_EQ(metrics.at("wall_ms"), 1.0);
}

// Malformed JSON should not silently discard a usable stdout line.
TEST(ParseMetricReportTest, FallsBackWhenTheJsonIsBroken) {
  MetricMap metrics;
  ASSERT_TRUE(ParseMetricReport("{not json", "RESULT wall_ms=5\n", &metrics));
  EXPECT_EQ(metrics.at("wall_ms"), 5.0);
}

TEST(ParseMetricReportTest, ReportsNothingWhenThereIsNothing) {
  MetricMap metrics;
  EXPECT_FALSE(ParseMetricReport("", "no numbers here\n", &metrics));
  EXPECT_FALSE(ParseMetricReport("", "", &metrics));
  EXPECT_FALSE(ParseMetricReport(R"({"metrics": {}})", "", &metrics));
}

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

}  // namespace
}  // namespace tournament_arena
