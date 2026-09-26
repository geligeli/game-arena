#include "game_arena/server/dashboard.h"

#include <unistd.h>

#include <filesystem>
#include <memory>
#include <string>

#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace tournament_arena {
namespace {

using ::testing::HasSubstr;
using ::testing::Not;
using GameRecord = tournament_broker::proto::GameRecord;

class DashboardTest : public ::testing::Test {
 protected:
  void SetUp() override {
    dir_ = std::filesystem::temp_directory_path() /
           ("dashboard_" + std::to_string(::getpid()));
    std::filesystem::remove_all(dir_);
    SubmissionRules rules;
    rules.files_submit_dir = "solutions";
    rules.harness.set_api_dep("//problem/harness:api");
    rules.harness.set_main_src("//problem/harness:main.cc");
    rules.policy.add_allow_paths("solutions/**");
    store_ = std::make_unique<CandidateStore>(dir_ / "candidates",
                                              CandidateLimits{}, rules);
    jobs_ = std::make_unique<JobLog>(dir_ / "jobs");
    games_ = std::make_unique<tournament_broker::GameHistory>(dir_ / "games");

    proto::SubmitRequest request;
    request.set_display_name("alice");
    request.set_game("nim");
    request.set_entry_header("strategy.h");
    auto *file = request.add_files();
    file->set_path("strategy.h");
    file->set_content("// alice's <strategy>\n");
    std::string error;
    const auto candidate = store_->Create(request, &error);
    ASSERT_TRUE(candidate.has_value()) << error;
    store_->SetStatus("alice", proto::Candidate::READY, "");

    JobRecord record;
    record.mutable_job()->set_job_id("j1_1");
    record.mutable_job()->set_candidate_id("alice");
    record.mutable_job()->set_state(proto::Job::DONE);
    *record.mutable_submission() = *candidate;
    JobRecord::Order *order = record.add_orders();
    order->set_order_id("o1_1");
    order->set_opponent_spec("builtin:random");
    order->mutable_result()->set_build_ok(true);
    order->mutable_result()->set_worker_id("w1");
    order->mutable_result()->set_build_output(
        "\x1b[1mINFO:\x1b[0m <script>alert(1)</script>\n");
    order->add_game_ids("o1_1-g1_0");
    jobs_->Put(record);

    Store("o1_1-g1_0", 2000, "alice");
  }

  void TearDown() override { std::filesystem::remove_all(dir_); }

  // A Nim game of three moves, won by seat 0.
  void Store(const std::string &game_id, int64_t finished_unix_ms,
             const std::string &seat0) {
    GameRecord record;
    record.set_game_id(game_id);
    record.set_game("nim");
    record.add_player_names(seat0);
    record.add_player_names("builtin:random");
    record.set_initial_state("3:0");
    record.set_initial_view("3 stones <start>");
    for (int taken = 1; taken <= 3; ++taken) {
      GameRecord::Step *step = record.add_steps();
      step->set_player((taken - 1) % 2);
      step->set_action("1");
      step->set_view(std::to_string(3 - taken) + " stones");
    }
    record.set_result(GameRecord::WIN);
    record.set_winning_player(0);
    record.set_termination_reason("normal");
    record.set_finished_unix_ms(finished_unix_ms);
    games_->Store(record);
  }

  std::string Page(std::string_view target, bool show_source = true) const {
    const Dashboard dashboard(store_.get(), jobs_.get(), games_.get(), nullptr,
                              show_source);
    const auto routed = dashboard.Route(target);
    EXPECT_TRUE(routed.has_value()) << target;
    return routed.has_value() ? routed->second : "";
  }

  bool Found(std::string_view target) const {
    const Dashboard dashboard(store_.get(), jobs_.get(), games_.get(), nullptr,
                              true);
    return dashboard.Route(target).has_value();
  }

  std::filesystem::path dir_;
  std::unique_ptr<CandidateStore> store_;
  std::unique_ptr<JobLog> jobs_;
  std::unique_ptr<tournament_broker::GameHistory> games_;
};

TEST_F(DashboardTest, ListsEveryJobWithItsBuild) {
  const std::string html = Page("/jobs");
  EXPECT_THAT(html, HasSubstr("<a href=\"/jobs/j1_1\">"));
  EXPECT_THAT(html, HasSubstr("<a href=\"/participants/alice\">"));
  EXPECT_THAT(html, HasSubstr("DONE"));
  EXPECT_THAT(html, HasSubstr("<td class=\"l\">ok</td>"));
}

TEST_F(DashboardTest, AJobShowsItsBuildOutputItsGamesAndItsSource) {
  const std::string html = Page("/jobs/j1_1");
  // Escaped, and without the compiler's colour codes.
  EXPECT_THAT(html, HasSubstr("INFO: &lt;script&gt;alert(1)&lt;/script&gt;"));
  EXPECT_THAT(html, Not(HasSubstr("<script>alert")));
  EXPECT_THAT(html, Not(HasSubstr("\x1b")));
  EXPECT_THAT(html, HasSubstr("<a href=\"/games/o1_1-g1_0\">1</a>"));
  EXPECT_THAT(html, HasSubstr("solutions/alice/strategy.h"));
  EXPECT_THAT(html, HasSubstr("// alice&#39;s &lt;strategy&gt;"));
}

TEST_F(DashboardTest, AParticipantHasTheirCodeAndEverySubmission) {
  const std::string html = Page("/participants/alice");
  EXPECT_THAT(html, HasSubstr("<a href=\"/jobs/j1_1\">"));
  EXPECT_THAT(html, HasSubstr("<a href=\"/games?player=alice\">"));
  EXPECT_THAT(html, HasSubstr("READY"));
  EXPECT_THAT(html, HasSubstr("// alice&#39;s &lt;strategy&gt;"));
}

TEST_F(DashboardTest, GamesAreNewestFirstAndFilteredByPlayer) {
  Store("o2_2-g2_0", 3000, "bob");
  Store("o0_0-g0_0", 1000, "alice");

  const std::string all = Page("/games");
  EXPECT_LT(all.find("/games/o2_2-g2_0"), all.find("/games/o1_1-g1_0"));
  EXPECT_LT(all.find("/games/o1_1-g1_0"), all.find("/games/o0_0-g0_0"));

  const std::string alices = Page("/games?player=alice");
  EXPECT_THAT(alices, HasSubstr("2 game(s)"));
  EXPECT_THAT(alices, Not(HasSubstr("o2_2-g2_0")));
}

TEST_F(DashboardTest, GamesArePagedAHundredAtATime) {
  for (int i = 0; i < 100; ++i) {
    Store("o9_" + std::to_string(i) + "-g", 5000 + i, "bob");
  }

  EXPECT_THAT(Page("/games"), HasSubstr("<a href=\"/games?page=1\">older</a>"));
  const std::string last = Page("/games?page=1");
  // The oldest game, and the way back.
  EXPECT_THAT(last, HasSubstr("/games/o1_1-g1_0"));
  EXPECT_THAT(last, HasSubstr("<a href=\"/games?page=0\">newer</a>"));
}

TEST_F(DashboardTest, AReplayHasAFrameForTheStartAndEveryMove) {
  const std::string html = Page("/games/o1_1-g1_0");
  std::size_t frames = 0;
  for (std::size_t at = html.find("class=\"f\""); at != std::string::npos;
       at = html.find("class=\"f\"", at + 1)) {
    ++frames;
  }
  EXPECT_EQ(frames, 4u);
  EXPECT_THAT(html, HasSubstr("3 stones &lt;start&gt;"));
  EXPECT_THAT(html, HasSubstr("seat 1 (builtin:random) played <code>1</code>"));
  EXPECT_THAT(html, HasSubstr("<pre>0 stones</pre>"));
  EXPECT_THAT(html, HasSubstr("<a href=\"/participants/alice\">alice</a> won"));
}

TEST_F(DashboardTest, UnknownAndUnsafeTargetsAreNotFound) {
  EXPECT_FALSE(Found("/jobs/nope"));
  EXPECT_FALSE(Found("/jobs/../candidates"));
  EXPECT_FALSE(Found("/games/../jobs/j1_1"));
  EXPECT_FALSE(Found("/participants/nobody"));
  EXPECT_FALSE(Found("/elsewhere"));
}

TEST_F(DashboardTest, TheSourcePolicyHidesSourceBuildOutputAndErrors) {
  const std::string job = Page("/jobs/j1_1", /*show_source=*/false);
  EXPECT_THAT(job, HasSubstr("Hidden by this problem"));
  EXPECT_THAT(job, Not(HasSubstr("INFO:")));
  EXPECT_THAT(job, Not(HasSubstr("strategy")));
  const std::string participant =
      Page("/participants/alice", /*show_source=*/false);
  EXPECT_THAT(participant, Not(HasSubstr("strategy")));
}

}  // namespace
}  // namespace tournament_arena
