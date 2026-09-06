#include "game_arena/grader/report.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>

#include "game_arena/sandbox/worker/metric_report.h"
#include "gtest/gtest.h"

namespace grader {
namespace {

using Metrics = std::map<std::string, double>;

auto ReadFile(const std::filesystem::path &path) -> std::string {
  std::ifstream in(path, std::ios::binary);
  std::ostringstream out;
  out << in.rdbuf();
  return out.str();
}

// The point of keeping both halves in one repo: what a grader writes is what
// the worker reads, held together by a test rather than by a comment.
TEST(Report, RoundTripsThroughTheWorkersParser) {
  const Metrics written{{"score", 87.5}, {"solved", 42}, {"wall_ms", 1234}};

  Metrics parsed;
  ASSERT_TRUE(tournament_arena::ParseMetricReport(RenderReport(written),
                                                  /*stdout_text=*/"", &parsed));
  EXPECT_EQ(parsed, written);
}

TEST(Report, RoundTripsAnEmptyReport) {
  Metrics parsed{{"stale", 1}};
  tournament_arena::ParseMetricReport(RenderReport({}), "", &parsed);
  EXPECT_TRUE(parsed.empty() || parsed.count("stale") == 0)
      << "an empty report must not leave a previous run's numbers behind";
}

TEST(Report, SurvivesValuesThatNaiveFormattingWouldMangle) {
  const Metrics written{{"tiny", 0.000001234},
                        {"huge", 9.87654321e12},
                        {"negative", -17.5},
                        {"zero", 0.0}};
  Metrics parsed;
  ASSERT_TRUE(
      tournament_arena::ParseMetricReport(RenderReport(written), "", &parsed));
  ASSERT_EQ(parsed.size(), written.size());
  for (const auto &[name, value] : written) {
    ASSERT_TRUE(parsed.count(name)) << name;
    EXPECT_DOUBLE_EQ(parsed.at(name), value) << name;
  }
}

TEST(WriteReport, WritesThePathItIsGiven) {
  const auto path = std::filesystem::temp_directory_path() / "report_test.json";
  std::filesystem::remove(path);

  std::string error;
  ASSERT_TRUE(WriteReport(path.string(), {{"score", 3}}, &error)) << error;
  EXPECT_NE(ReadFile(path).find("\"score\""), std::string::npos);
  std::filesystem::remove(path);
}

TEST(WriteReport, FailsLoudlyOnAnUnwritablePath) {
  std::string error;
  EXPECT_FALSE(WriteReport("/nonexistent-dir/report.json", {{"a", 1}}, &error));
  EXPECT_FALSE(error.empty());
}

// A grader run outside the arena should say so rather than silently score
// nothing.
TEST(WriteReportToArenaPath, FailsWhenTheVariableIsUnset) {
  ::unsetenv("ARENA_REPORT");
  std::string error;
  EXPECT_FALSE(WriteReportToArenaPath({{"a", 1}}, &error));
  EXPECT_NE(error.find("ARENA_REPORT"), std::string::npos);
}

TEST(WriteReportToArenaPath, UsesTheVariableWhenSet) {
  const auto path = std::filesystem::temp_directory_path() / "arena_env.json";
  std::filesystem::remove(path);
  ::setenv("ARENA_REPORT", path.string().c_str(), 1);

  std::string error;
  ASSERT_TRUE(WriteReportToArenaPath({{"score", 9}}, &error)) << error;
  EXPECT_NE(ReadFile(path).find("\"score\""), std::string::npos);

  ::unsetenv("ARENA_REPORT");
  std::filesystem::remove(path);
}

}  // namespace
}  // namespace grader
