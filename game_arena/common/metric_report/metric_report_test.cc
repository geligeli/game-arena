// The $ARENA_REPORT contract, from the reading end. game_arena/grader's
// report_test.cc exercises the writing end against this same parser.

#include "game_arena/common/metric_report/metric_report.h"

#include <map>
#include <string>

#include "gtest/gtest.h"

namespace metric_report {
namespace {

using MetricMap = std::map<std::string, double>;

TEST(ParseMetricReportTest, ReadsTheJsonReport) {
  MetricMap metrics;
  ASSERT_TRUE(Parse(R"({"metrics": {"wall_ms": 1234.5, "peak_rss_mb": 91.25}})",
                    "", &metrics));
  EXPECT_EQ(metrics.at("wall_ms"), 1234.5);
  EXPECT_EQ(metrics.at("peak_rss_mb"), 91.25);
}

// A benchmark reporting more than the problem ranks on is normal; failing the
// run over it would be hostile.
TEST(ParseMetricReportTest, ToleratesUnknownJsonFields) {
  MetricMap metrics;
  ASSERT_TRUE(
      Parse(R"({"metrics": {"wall_ms": 10}, "notes": "warm cache", "runs": 3})",
            "", &metrics));
  EXPECT_EQ(metrics.at("wall_ms"), 10.0);
}

// A benchmark that only knows how to print a line is not shut out.
TEST(ParseMetricReportTest, FallsBackToTheResultLine) {
  MetricMap metrics;
  ASSERT_TRUE(Parse("",
                    "building...\nRESULT wall_ms=250.5 peak_rss_mb=64\ndone\n",
                    &metrics));
  EXPECT_EQ(metrics.at("wall_ms"), 250.5);
  EXPECT_EQ(metrics.at("peak_rss_mb"), 64.0);
}

TEST(ParseMetricReportTest, TheLastResultLineWins) {
  MetricMap metrics;
  ASSERT_TRUE(Parse("", "RESULT wall_ms=900\nRESULT wall_ms=100\n", &metrics));
  EXPECT_EQ(metrics.at("wall_ms"), 100.0);
}

TEST(ParseMetricReportTest, PrefersTheJsonReportOverStdout) {
  MetricMap metrics;
  ASSERT_TRUE(Parse(R"({"metrics": {"wall_ms": 1}})", "RESULT wall_ms=999\n",
                    &metrics));
  EXPECT_EQ(metrics.at("wall_ms"), 1.0);
}

// Malformed JSON should not silently discard a usable stdout line.
TEST(ParseMetricReportTest, FallsBackWhenTheJsonIsBroken) {
  MetricMap metrics;
  ASSERT_TRUE(Parse("{not json", "RESULT wall_ms=5\n", &metrics));
  EXPECT_EQ(metrics.at("wall_ms"), 5.0);
}

TEST(ParseMetricReportTest, ReportsNothingWhenThereIsNothing) {
  MetricMap metrics;
  EXPECT_FALSE(Parse("", "no numbers here\n", &metrics));
  EXPECT_FALSE(Parse("", "", &metrics));
  EXPECT_FALSE(Parse(R"({"metrics": {}})", "", &metrics));
}

}  // namespace
}  // namespace metric_report
