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
    proto::SubmissionPolicy rules;
    rules.set_files_submit_dir("solutions");
    rules.mutable_harness()->set_api_dep("//problem/harness:api");
    rules.mutable_harness()->set_main_src("//problem/harness:main.cc");
    rules.add_allow_paths("solutions/**");
    store_ = std::make_unique<CandidateStore>(dir_ / "candidates",
                                              CandidateLimits{}, rules);
    jobs_ = std::make_unique<JobLog>(dir_ / "jobs");
    games_ = std::make_unique<tournament_broker::GameHistory>(dir_ / "games");

    proto::SubmitRequest request;
    request.set_display_name("alice");
    request.set_game("nim");
    request.set_entry_header("strategy.h");
    auto* file = request.add_files();
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
    JobRecord::Order* order = record.add_orders();
    order->set_order_id("o1_1");
    order->add_opponent_spec("builtin:random");
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
  void Store(const std::string& game_id, int64_t finished_unix_ms,
             const std::string& seat0) {
    GameRecord record;
    record.set_game_id(game_id);
    record.set_game("nim");
    record.add_player_names(seat0);
    record.add_player_names("builtin:random");
    record.set_initial_state("3:0");
    record.set_initial_view("3 stones <start>");
    for (int taken = 1; taken <= 3; ++taken) {
      GameRecord::Step* step = record.add_steps();
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

TEST_F(DashboardTest, AMatchWithNoPatchShowsTheCandidatesCode) {
  JobRecord record;
  record.mutable_job()->set_job_id("j2_2");
  record.mutable_job()->set_candidate_id("alice");
  jobs_->Put(record);
  EXPECT_THAT(Page("/jobs/j2_2"), HasSubstr("// alice&#39;s &lt;strategy&gt;"));
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
  EXPECT_THAT(html, HasSubstr(">0 stones</pre>"));
  EXPECT_THAT(html, HasSubstr("<a href=\"/participants/alice\">alice</a> won"));
}

// A view may colour itself with ANSI SGR; any other control byte is not text.
TEST_F(DashboardTest, AReplayShowsAnsiColouredViewsAsSpans) {
  GameRecord record;
  record.set_game_id("o1_1-g2_0");
  record.set_game("nim");
  record.add_player_names("alice");
  record.add_player_names("builtin:random");
  record.set_initial_view("\x1b[41m<red>\x1b[0m plain");
  GameRecord::Step* step = record.add_steps();
  step->set_player(0);
  step->set_action("1");
  step->set_view("\x1b]0;title\x07");
  record.set_result(GameRecord::WIN);
  games_->Store(record);

  const std::string html = Page("/games/o1_1-g2_0");
  EXPECT_THAT(html, HasSubstr("<span style=\"background:#c33;\">&lt;red&gt;"
                              "</span> plain"));
  EXPECT_THAT(html, HasSubstr("(10 bytes, not text)"));
}

// A caption replaces the action's bytes, and an empty view keeps showing the
// one before it.
TEST_F(DashboardTest, AReplayShowsCaptionsAndKeepsTheLastView) {
  GameRecord record;
  record.set_game_id("o1_1-g3_0");
  record.set_game("nim");
  record.add_player_names("alice");
  record.add_player_names("builtin:random");
  record.set_initial_view("board 0");
  for (const auto& [caption, view] :
       {std::pair{"alice takes 2", "board 1"}, std::pair{"random takes 1", ""},
        std::pair{"", ""}}) {
    GameRecord::Step* step = record.add_steps();
    step->set_player(0);
    step->set_action("\xff");
    step->set_caption(caption);
    step->set_view(view);
  }
  record.set_result(GameRecord::WIN);
  games_->Store(record);

  const std::string html = Page("/games/o1_1-g3_0");
  EXPECT_THAT(html, HasSubstr("seat 0 (alice): alice takes 2</p>"));
  EXPECT_THAT(html, Not(HasSubstr("not text")));
  // Step 2 has no view of its own: it shows step 1's.
  EXPECT_THAT(html,
              HasSubstr("data-v=\"1\" data-p=\"0\" data-own=\"1\"><p>Move 1:"));
  EXPECT_THAT(html, HasSubstr("data-v=\"1\" data-p=\"0\"><p>Move 2:"));
  EXPECT_THAT(html, HasSubstr("data-v=\"1\" data-p=\"0\"><p>Move 3:"));
  EXPECT_THAT(html, HasSubstr("id=\"speed\""));
  EXPECT_THAT(html, HasSubstr("location.hash")) << "#<move> links a frame";
  EXPECT_THAT(html, HasSubstr("seek(1)"));
}

// Any file a problem ships is served by its name; the extension only picks
// the Content-Type a browser needs for a module script or streamed wasm.
TEST_F(DashboardTest, ServesReplayAssetsWhateverTheyAre) {
  const Dashboard dashboard(
      store_.get(), jobs_.get(), games_.get(), nullptr, true,
      ReplayAssets{.files = {{"nim.js", "export function render() {}"},
                             {"nim.wasm", std::string("\0asm", 4)},
                             {"board.svg", "<svg/>"},
                             {"notes.xyz", "anything"}},
                   .module = "nim.js"});
  const auto js = dashboard.Route("/assets/nim.js?v=3");
  ASSERT_TRUE(js.has_value());
  EXPECT_EQ(js->first, "text/javascript; charset=utf-8");
  EXPECT_EQ(js->second, "export function render() {}");
  EXPECT_EQ(dashboard.Route("/assets/nim.wasm")->first, "application/wasm");
  EXPECT_EQ(dashboard.Route("/assets/nim.wasm")->second.size(), 4u);
  EXPECT_EQ(dashboard.Route("/assets/board.svg")->first, "image/svg+xml");
  EXPECT_EQ(dashboard.Route("/assets/notes.xyz")->first,
            "application/octet-stream");
  EXPECT_FALSE(dashboard.Route("/assets/missing.js").has_value());
  EXPECT_FALSE(dashboard.Route("/assets/../jobs/j1_1").has_value());
  EXPECT_FALSE(Found("/assets/nim.js")) << "no assets without the problem's";
}

// With a module the page hands it each view's bytes: base64 in #views, the
// game and players in #game, and no text views of its own.
TEST_F(DashboardTest, AReplayWithAModuleHandsItTheViews) {
  GameRecord record;
  record.set_game_id("o1_1-g4_0");
  record.set_game("nim");
  record.add_player_names("</script><b>");
  record.add_player_names("builtin:random");
  record.set_initial_view("board 0");
  GameRecord::Step* step = record.add_steps();
  step->set_player(0);
  step->set_action("1");
  step->set_view(std::string("\x01\xff", 2));
  record.add_steps()->set_player(1);
  record.set_result(GameRecord::WIN);
  games_->Store(record);

  const Dashboard dashboard(
      store_.get(), jobs_.get(), games_.get(), nullptr, true,
      ReplayAssets{.files = {{"nim.js", ""}}, .module = "nim.js"});
  const std::string html = dashboard.Route("/games/o1_1-g4_0")->second;
  EXPECT_THAT(html, HasSubstr("<div id=\"stage\">"));
  EXPECT_THAT(html, HasSubstr(">[\"Ym9hcmQgMA==\",\"Af8=\"]</script>"));
  EXPECT_THAT(html, HasSubstr("\"module\":\"/assets/nim.js\""));
  // init sees every view, render which one it draws.
  EXPECT_THAT(html, HasSubstr("m.init(stage,game,raw.map("));
  EXPECT_THAT(html, HasSubstr("{index:i,view:v,"));
  // A name cannot close the script it sits in.
  EXPECT_THAT(html, HasSubstr("\"\\u003c/script\\u003e\\u003cb\\u003e\""));
  EXPECT_THAT(html, Not(HasSubstr("<pre class=\"v\"")));
  EXPECT_THAT(html, HasSubstr("data-p=\"1\""));
}

// Past two seats a result is the placings, and a forfeit says who and why.
TEST_F(DashboardTest, AFreeForAllShowsPlacingsAndForfeits) {
  GameRecord record;
  record.set_game_id("o1_1-g3_0");
  record.set_game("nim3");
  for (const char* name : {"alice", "bob", "carol"}) {
    record.add_player_names(name);
  }
  record.set_result(GameRecord::WIN);
  record.set_winning_player(1);
  for (const int place : {2, 0, 1}) {
    record.add_places(place);
  }
  GameRecord::Forfeit* forfeit = record.add_forfeits();
  forfeit->set_seat(0);
  forfeit->set_reason("timeout");
  forfeit->set_move(4);
  record.set_termination_reason("normal");
  record.set_finished_unix_ms(3000);
  games_->Store(record);

  const std::string games = Page("/games");
  EXPECT_NE(games.find("1st <a href=\"/participants/bob\">bob</a>, 2nd "),
            std::string::npos)
      << games;
  const std::string replay = Page("/games/o1_1-g3_0");
  EXPECT_NE(replay.find("3rd <a href=\"/participants/alice\">alice</a>"),
            std::string::npos);
  EXPECT_NE(replay.find("forfeited (timeout) at move 4"), std::string::npos)
      << replay;
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
