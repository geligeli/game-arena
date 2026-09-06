// Standings for both kinds of problem.
//
// The ELO half carries a regression this codebase actually had: when the
// referee moved into the sandbox, its ratings went with it, and for a while
// nothing wrote the coordinator's store at all. A leaderboard that quietly
// shows everyone at 1500 looks fine. RecordsATallyIntoTheRatingStore is what
// makes that loud.

#include <gtest/gtest.h>
#include <unistd.h>

#include <filesystem>
#include <memory>
#include <string>

#include "game_mcts/tournament_server/server/elo_standings.h"
#include "game_mcts/tournament_server/server/metric_standings.h"

namespace tournament_arena {
namespace {

auto TempDir(const std::string &tag) -> std::filesystem::path {
  const auto dir =
      std::filesystem::temp_directory_path() /
      (tag + "_" + std::to_string(::getpid()) + "_" +
       std::to_string(::testing::UnitTest::GetInstance()->random_seed()));
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  return dir;
}

auto Tally(int wins, int draws, int losses) -> proto::OrderResult {
  proto::OrderResult result;
  result.set_build_ok(true);
  result.set_wins(wins);
  result.set_draws(draws);
  result.set_losses(losses);
  result.set_games_played(wins + draws + losses);
  return result;
}

auto Metrics(std::initializer_list<std::pair<std::string, double>> values,
             const std::string &worker = "w1",
             const std::string &machine = "bench-c7i") -> proto::OrderResult {
  proto::OrderResult result;
  result.set_build_ok(true);
  for (const auto &[name, value] : values) {
    (*result.mutable_metrics())[name] = value;
  }
  result.set_worker_id(worker);
  result.set_machine_class(machine);
  result.set_games_played(3);
  return result;
}

class EloStandingsTest : public ::testing::Test {
 protected:
  void SetUp() override {
    dir_ = TempDir("elo_standings");
    elo_ = std::make_unique<tournament_broker::EloStore>(dir_ / "ratings.pb",
                                                         32.0);
    SubmissionRules rules;
    rules.files_submit_dir = "solutions";
    rules.policy.add_allowed_dep_prefixes("//problem/lib:");
    rules.policy.add_allowed_dep_prefixes("//game_mcts/core/mcts:");
    rules.policy.add_allowed_dep_prefixes("//game_mcts/games/risk:");
    rules.policy.add_allowed_dep_prefixes("//game_mcts/games/risk/strategies:");
    rules.policy.add_allowed_dep_prefixes("@abseil-cpp//");
    rules.harness.set_api_dep("//problem/harness:api");
    rules.harness.set_main_src("//problem/harness:main.cc");
    store_ = std::make_unique<CandidateStore>(dir_ / "candidates",
                                              CandidateLimits{}, rules);
    standings_ =
        std::make_unique<EloStandings>(elo_.get(), store_.get(), "risk2");
  }
  void TearDown() override { std::filesystem::remove_all(dir_); }

  auto Add(const std::string &name) -> std::string {
    proto::SubmitRequest request;
    request.set_display_name(name);
    request.set_entry_header("strategy.h");
    auto *file = request.add_files();
    file->set_path("strategy.h");
    file->set_content("// " + name + "\n");
    std::string error;
    const auto candidate = store_->Create(request, "commit0", &error);
    EXPECT_TRUE(candidate.has_value()) << error;
    store_->SetStatus(candidate->candidate_id(), proto::Candidate::READY, "");
    return candidate->candidate_id();
  }

  std::filesystem::path dir_;
  std::unique_ptr<tournament_broker::EloStore> elo_;
  std::unique_ptr<CandidateStore> store_;
  std::unique_ptr<EloStandings> standings_;
};

// The coordinator owns the ratings. The referee's own store dies with its
// container, so if this does not write, nothing does.
TEST_F(EloStandingsTest, RecordsATallyIntoTheRatingStore) {
  const std::string alpha = Add("Alpha");
  EXPECT_FALSE(standings_->has(alpha)) << "unplayed is not a result";

  standings_->Record(alpha, "builtin:random", Tally(3, 0, 1));

  EXPECT_TRUE(standings_->has(alpha));
  const Standing standing = standings_->Get(alpha);
  EXPECT_EQ(standing.wins, 3);
  EXPECT_EQ(standing.losses, 1);
  EXPECT_GT(standing.score, 1500.0)
      << "winning more than losing should rate up";
  // The opponent is a player too, which is what makes "beat builtin:mcts"
  // something to be rated for.
  EXPECT_LT(elo_->Get("risk2", "builtin:random").elo(), 1500.0);
}

TEST_F(EloStandingsTest, RatesACandidateOpponentUnderItsOwnId) {
  const std::string alpha = Add("Alpha");
  const std::string beta = Add("Beta");
  standings_->Record(alpha, "player:" + beta, Tally(2, 0, 0));

  EXPECT_GT(standings_->Get(alpha).score, 1500.0);
  // The "player:" prefix is routing, not part of the name.
  EXPECT_LT(standings_->Get(beta).score, 1500.0);
  EXPECT_EQ(elo_->Get("risk2", "player:" + beta).wins(), 0);
}

// ELO is path dependent: each game updates the rating the next one is scored
// against. Applying a tally in one lump would give a different number.
TEST_F(EloStandingsTest, AppliesEachGameSeparately) {
  const std::string alpha = Add("Alpha");
  standings_->Record(alpha, "builtin:random", Tally(1, 0, 0));
  const double after_one = standings_->Get(alpha).score;

  const std::string beta = Add("Beta");
  standings_->Record(beta, "builtin:mcts", Tally(2, 0, 0));
  const double after_two = standings_->Get(beta).score;

  EXPECT_GT(after_two, after_one) << "two wins should out-rate one";
  EXPECT_EQ(standings_->Get(beta).wins, 2);
}

TEST_F(EloStandingsTest, RanksReadyCandidatesBestFirst) {
  const std::string weak = Add("Weak");
  const std::string strong = Add("Strong");
  standings_->Record(weak, "builtin:random", Tally(0, 0, 5));
  standings_->Record(strong, "builtin:random", Tally(5, 0, 0));

  const auto rows = standings_->Rank(0);
  ASSERT_EQ(rows.size(), 2u);
  EXPECT_EQ(rows[0].candidate_id, strong);
  EXPECT_EQ(rows[1].candidate_id, weak);
  EXPECT_EQ(standings_->Rank(1).size(), 1u);
}

class MetricStandingsTest : public ::testing::Test {
 protected:
  void SetUp() override { dir_ = TempDir("metric_standings"); }
  void TearDown() override { std::filesystem::remove_all(dir_); }
  std::filesystem::path dir_;
};

TEST_F(MetricStandingsTest, RanksLowerAsBetterWhenMinimizing) {
  MetricStandings standings(dir_ / "metrics.pb", "wall_ms",
                            /*lower_is_better=*/true);
  standings.Record("slow", "", Metrics({{"wall_ms", 900.0}}));
  standings.Record("fast", "", Metrics({{"wall_ms", 100.0}}));

  const auto rows = standings.Rank(0);
  ASSERT_EQ(rows.size(), 2u);
  EXPECT_EQ(rows[0].candidate_id, "fast");
  EXPECT_EQ(rows[0].score, 100.0);
  EXPECT_EQ(standings.score_label(), "wall_ms");
}

TEST_F(MetricStandingsTest, RanksHigherAsBetterWhenMaximizing) {
  MetricStandings standings(dir_ / "metrics.pb", "accuracy",
                            /*lower_is_better=*/false);
  standings.Record("poor", "", Metrics({{"accuracy", 0.6}}));
  standings.Record("good", "", Metrics({{"accuracy", 0.95}}));

  const auto rows = standings.Rank(0);
  ASSERT_EQ(rows.size(), 2u);
  EXPECT_EQ(rows[0].candidate_id, "good");
}

// A wall-clock number is not a property of the submission alone.
TEST_F(MetricStandingsTest, KeepsTheHostThatProducedTheNumber) {
  MetricStandings standings(dir_ / "metrics.pb", "wall_ms", true);
  standings.Record("x", "",
                   Metrics({{"wall_ms", 120.0}, {"peak_rss_mb", 91.0}},
                           "worker-7", "bench-c7i"));

  const Standing standing = standings.Get("x");
  EXPECT_EQ(standing.worker_id, "worker-7");
  EXPECT_EQ(standing.machine_class, "bench-c7i");
  // Secondary metrics are kept and shown, they just do not order the board.
  EXPECT_EQ(standing.metrics.at("peak_rss_mb"), 91.0);
  EXPECT_EQ(standing.score, 120.0);
}

// The newest measurement stands; a stale metric alongside a fresh one would
// rank a submission on numbers taken at different times.
TEST_F(MetricStandingsTest, ReplacesRatherThanMergesOnRemeasure) {
  MetricStandings standings(dir_ / "metrics.pb", "wall_ms", true);
  standings.Record("x", "", Metrics({{"wall_ms", 500.0}, {"gone", 1.0}}));
  standings.Record("x", "", Metrics({{"wall_ms", 300.0}}));

  const Standing standing = standings.Get("x");
  EXPECT_EQ(standing.score, 300.0);
  EXPECT_FALSE(standing.metrics.contains("gone"));
}

// A submission with no reading for the ranked metric cannot be placed. Showing
// it at one end would read as a score it never earned.
TEST_F(MetricStandingsTest, OmitsSubmissionsMissingTheRankedMetric) {
  MetricStandings standings(dir_ / "metrics.pb", "wall_ms", true);
  standings.Record("measured", "", Metrics({{"wall_ms", 100.0}}));
  standings.Record("other", "", Metrics({{"something_else", 3.0}}));

  const auto rows = standings.Rank(0);
  ASSERT_EQ(rows.size(), 1u);
  EXPECT_EQ(rows[0].candidate_id, "measured");
  EXPECT_TRUE(standings.has("other")) << "recorded, just not rankable";
}

TEST_F(MetricStandingsTest, SurvivesARestart) {
  const auto path = dir_ / "metrics.pb";
  {
    MetricStandings standings(path, "wall_ms", true);
    standings.Record("x", "", Metrics({{"wall_ms", 42.0}}));
  }
  MetricStandings reopened(path, "wall_ms", true);
  reopened.Load();
  EXPECT_EQ(reopened.Get("x").score, 42.0);
  EXPECT_EQ(reopened.Rank(0).size(), 1u);
}

TEST_F(MetricStandingsTest, IgnoresAResultWithNoMetrics) {
  MetricStandings standings(dir_ / "metrics.pb", "wall_ms", true);
  proto::OrderResult empty;
  empty.set_build_ok(true);
  standings.Record("x", "", empty);
  EXPECT_FALSE(standings.has("x"));
}

}  // namespace
}  // namespace tournament_arena
