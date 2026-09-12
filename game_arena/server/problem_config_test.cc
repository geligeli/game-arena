#include "game_arena/server/problem_config.h"

#include <gtest/gtest.h>

#include <fstream>
#include <string>

namespace tournament_arena {
namespace {

// A config with every required field of a match problem and nothing else, so a
// test can delete one line and assert on the resulting error.
constexpr char kMatchConfig[] = R"pb(
  problem_id: "risk2"
  display_name: "Risk, two players"
  repo { url: "/repo" base_commit: "abc123" }
  build { targets: "//bot" }
  match { game: "risk2" referee_target: "//referee:match_referee" }
  ranking { kind: ELO }
)pb";

constexpr char kGradeConfig[] = R"pb(
  problem_id: "mcts-bench"
  repo { url: "/repo" }
  build { targets: "//bench" }
  grade {
    argv: "bazel-bin/bench"
    metrics { name: "wall_ms" direction: MINIMIZE primary: true }
    metrics { name: "peak_rss_mb" direction: MINIMIZE }
  }
  ranking { kind: METRIC }
)pb";

auto Load(const std::string &text,
          std::string *error) -> std::optional<proto::ProblemConfig> {
  std::optional<proto::ProblemConfig> config =
      ParseProblemConfigText(text, error);
  if (!config) {
    return std::nullopt;
  }
  ApplyProblemDefaults(&*config);
  if (!ValidateProblemConfig(*config, error)) {
    return std::nullopt;
  }
  return config;
}

TEST(ProblemConfigTest, ParsesAMatchProblem) {
  std::string error;
  const auto config = Load(kMatchConfig, &error);
  ASSERT_TRUE(config.has_value()) << error;
  EXPECT_EQ(config->problem_id(), "risk2");
  EXPECT_EQ(config->match().game(), "risk2");
  EXPECT_EQ(config->ranking().kind(), proto::RankingSpec::ELO);
}

TEST(ProblemConfigTest, ParsesAGradeProblem) {
  std::string error;
  const auto config = Load(kGradeConfig, &error);
  ASSERT_TRUE(config.has_value()) << error;
  ASSERT_NE(PrimaryMetric(*config), nullptr);
  EXPECT_EQ(PrimaryMetric(*config)->name(), "wall_ms");
  EXPECT_EQ(PrimaryMetric(*config)->direction(), proto::MetricSpec::MINIMIZE);
}

TEST(ProblemConfigTest, ReportsParseErrorsWithLineNumbers) {
  std::string error;
  EXPECT_FALSE(
      ParseProblemConfigText("problem_id: \"a\"\nnot_a_field: 3\n", &error));
  EXPECT_NE(error.find("line 2"), std::string::npos) << error;
}

// An unknown field is a typo or a config from a newer server. Accepting it
// would silently apply a default the author believed they had overridden.
TEST(ProblemConfigTest, RejectsUnknownFields) {
  std::string error;
  EXPECT_FALSE(ParseProblemConfigText("problem_i: \"typo\"", &error));
}

TEST(ProblemConfigTest, AppliesDefaultsAndIsIdempotent) {
  std::string error;
  auto config = Load(kMatchConfig, &error);
  ASSERT_TRUE(config.has_value()) << error;

  EXPECT_EQ(config->submission().max_patch_bytes(), 2u * 1024 * 1024);
  EXPECT_EQ(config->build().timeout_s(), 1800u);
  EXPECT_EQ(config->sandbox().memory_limit_mb(), 4096u);
  EXPECT_EQ(config->match().games_per_order(), 10u);
  EXPECT_EQ(config->clients().default_quota().max_active_evaluations(), 1u);
  EXPECT_EQ(config->clients().default_quota().max_queued_jobs(), 8u);

  const std::string once = config->DebugString();
  ApplyProblemDefaults(&*config);
  EXPECT_EQ(config->DebugString(), once);
}

TEST(ProblemConfigTest, KeepsExplicitValuesOverDefaults) {
  std::string error;
  const auto config =
      Load(R"pb(
             problem_id: "risk2"
             repo { url: "/repo" }
             build { targets: "//bot" timeout_s: 60 }
             match { game: "risk2" referee_target: "//r" games_per_order: 2 }
             ranking { kind: ELO }
           )pb",
           &error);
  ASSERT_TRUE(config.has_value()) << error;
  EXPECT_EQ(config->build().timeout_s(), 60u);
  EXPECT_EQ(config->match().games_per_order(), 2u);
  // Unset base_commit means "the tree the server is started on".
  EXPECT_EQ(config->repo().base_commit(), "HEAD");
}

TEST(ProblemConfigTest, RejectsUnusableProblemIds) {
  EXPECT_TRUE(IsValidProblemId("risk2"));
  EXPECT_TRUE(IsValidProblemId("mcts-bench_v2"));
  EXPECT_FALSE(IsValidProblemId(""));
  EXPECT_FALSE(IsValidProblemId("Risk2")) << "uppercase is not a valid key";
  EXPECT_FALSE(IsValidProblemId("-leading"));
  EXPECT_FALSE(IsValidProblemId("has/slash")) << "must be one path component";
  EXPECT_FALSE(IsValidProblemId(".."));
  EXPECT_FALSE(IsValidProblemId(std::string(65, 'a')));
}

TEST(ProblemConfigTest, ExpandsSubmissionIdInTargets) {
  EXPECT_EQ(
      ExpandSubmissionId("//solutions/{submission_id}:bot", "fast-a1b2c3"),
      "//solutions/fast-a1b2c3:bot");
  // Every occurrence, not just the first.
  EXPECT_EQ(ExpandSubmissionId("{submission_id}/{submission_id}", "x"), "x/x");
  EXPECT_EQ(ExpandSubmissionId("//no/placeholder:bot", "x"),
            "//no/placeholder:bot");
  EXPECT_EQ(ExpandSubmissionId("", "x"), "");
}

TEST(ProblemConfigTest, RequiresAnEvaluation) {
  std::string error;
  EXPECT_FALSE(Load(R"pb(
                      problem_id: "p"
                      repo { url: "/repo" }
                      build { targets: "//bot" }
                    )pb",
                    &error));
  EXPECT_NE(error.find("grade or match"), std::string::npos) << error;
}

TEST(ProblemConfigTest, RequiresRankingToMatchEvaluation) {
  std::string error;
  EXPECT_FALSE(Load(R"pb(
                      problem_id: "p"
                      repo { url: "/repo" }
                      build { targets: "//bench" }
                      grade {
                        argv: "run"
                        metrics { name: "wall_ms" primary: true }
                      }
                      ranking { kind: ELO }
                    )pb",
                    &error));
  EXPECT_NE(error.find("ELO requires a match"), std::string::npos) << error;

  EXPECT_FALSE(Load(R"pb(
                      problem_id: "p"
                      repo { url: "/repo" }
                      build { targets: "//bot" }
                      match { game: "g" referee_target: "//r" }
                      ranking { kind: METRIC }
                    )pb",
                    &error));
  EXPECT_NE(error.find("METRIC requires a grade"), std::string::npos) << error;
}

TEST(ProblemConfigTest, RequiresExactlyOnePrimaryMetric) {
  std::string error;
  EXPECT_FALSE(Load(R"pb(
                      problem_id: "p"
                      repo { url: "/repo" }
                      build { targets: "//bench" }
                      grade {
                        argv: "run"
                        metrics { name: "a" }
                        metrics { name: "b" }
                      }
                      ranking { kind: METRIC }
                    )pb",
                    &error));
  EXPECT_NE(error.find("primary"), std::string::npos) << error;
}

TEST(ProblemConfigTest, RejectsRankingByAnUndeclaredMetric) {
  std::string error;
  EXPECT_FALSE(Load(R"pb(
                      problem_id: "p"
                      repo { url: "/repo" }
                      build { targets: "//bench" }
                      grade {
                        argv: "run"
                        metrics { name: "wall_ms" primary: true }
                      }
                      ranking { kind: METRIC metric_name: "typo_ms" }
                    )pb",
                    &error));
  EXPECT_NE(error.find("typo_ms"), std::string::npos) << error;
}

TEST(ProblemConfigTest, RequiresAnImageWhenContainersAreMandatory) {
  std::string error;
  EXPECT_FALSE(Load(R"pb(
                      problem_id: "p"
                      repo { url: "/repo" }
                      build { targets: "//bot" }
                      sandbox { require_container: true }
                      match { game: "g" referee_target: "//r" }
                      ranking { kind: ELO }
                    )pb",
                    &error));
  EXPECT_NE(error.find("sandbox.image"), std::string::npos) << error;
}

TEST(ProblemConfigTest, ResolvesARelativeRepoUrlAgainstTheConfigDir) {
  std::string error;
  auto config = ParseProblemConfigText(kMatchConfig, &error);
  ASSERT_TRUE(config.has_value()) << error;

  config->mutable_repo()->set_url(".");
  ResolveRelativeRepoUrl(&*config, "/srv/problems/risk2");
  EXPECT_EQ(config->repo().url(), "/srv/problems/risk2");

  config->mutable_repo()->set_url("../shared");
  ResolveRelativeRepoUrl(&*config, "/srv/problems/risk2");
  EXPECT_EQ(config->repo().url(), "/srv/problems/shared");

  // Absolute paths and anything URL-shaped are not the config's business.
  for (const char *untouched :
       {"/abs/repo", "https://github.com/x/y.git", "git@github.com:x/y.git",
        "ssh://git@host/x.git", "file:///srv/x"}) {
    config->mutable_repo()->set_url(untouched);
    ResolveRelativeRepoUrl(&*config, "/srv/problems/risk2");
    EXPECT_EQ(config->repo().url(), untouched);
  }
}

TEST(ProblemConfigTest, LoadFromFileResolvesRepoUrlDot) {
  const std::filesystem::path dir =
      std::filesystem::temp_directory_path() / "problem_config_test_dot";
  std::filesystem::create_directories(dir);
  const std::filesystem::path path = dir / "problem.textproto";
  {
    std::ofstream out(path);
    out << "problem_id: \"p\" repo { url: \".\" } build { targets: \"//x\" }"
           " match { game: \"g\" referee_target: \"//r\" } ranking { kind: ELO "
           "}";
  }
  std::string error;
  const auto config = LoadProblemConfig(path, &error);
  ASSERT_TRUE(config.has_value()) << error;
  EXPECT_EQ(config->repo().url(), std::filesystem::canonical(dir).string());
  std::filesystem::remove_all(dir);
}

TEST(ProblemConfigTest, LoadFromFileReportsThePath) {
  const std::filesystem::path path =
      std::filesystem::temp_directory_path() / "problem_config_test.textproto";
  {
    std::ofstream out(path);
    out << kMatchConfig;
  }
  std::string error;
  const auto config = LoadProblemConfig(path, &error);
  ASSERT_TRUE(config.has_value()) << error;
  EXPECT_EQ(config->problem_id(), "risk2");
  std::filesystem::remove(path);

  EXPECT_FALSE(LoadProblemConfig("/nonexistent/problem.textproto", &error));
  EXPECT_NE(error.find("/nonexistent/problem.textproto"), std::string::npos);
}

}  // namespace
}  // namespace tournament_arena
