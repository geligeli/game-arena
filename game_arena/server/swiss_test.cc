#include "game_arena/server/swiss.h"

#include <unistd.h>

#include <filesystem>

#include "game_arena/standings/game_history.h"
#include "gtest/gtest.h"

namespace tournament_arena {
namespace {

using Pairs = std::vector<std::pair<std::string, std::string>>;

TEST(SwissPairsTest, NeighboursMeetAndTheLowestTakesTheBye) {
  const SwissRound round = SwissPairs({"a", "b", "c", "d", "e"}, {}, {});
  EXPECT_EQ(round.pairs, (Pairs{{"a", "b"}, {"c", "d"}}));
  EXPECT_EQ(round.bye, "e");
}

TEST(SwissPairsTest, NoRematchWhileAFreshOpponentIsLeft) {
  const SwissRound round =
      SwissPairs({"a", "b", "c", "d"}, {{"a", "b"}, {"c", "d"}}, {});
  EXPECT_EQ(round.pairs, (Pairs{{"a", "c"}, {"b", "d"}}));
}

TEST(SwissPairsTest, ARematchRatherThanNoGame) {
  const SwissRound round = SwissPairs({"a", "b"}, {{"a", "b"}}, {});
  EXPECT_EQ(round.pairs, (Pairs{{"a", "b"}}));
}

TEST(SwissPairsTest, NoSecondByeWhileSomeoneHasHadNone) {
  const SwissRound round = SwissPairs({"a", "b", "c"}, {}, {"c"});
  EXPECT_EQ(round.bye, "b");
  EXPECT_EQ(round.pairs, (Pairs{{"a", "c"}}));
}

class SeedVersionsTest : public ::testing::Test {
 protected:
  void SetUp() override {
    dir_ = std::filesystem::temp_directory_path() /
           ("swiss_" + std::to_string(::getpid()));
    std::filesystem::remove_all(dir_);
    rules_.set_files_submit_dir("solutions");
    rules_.add_allow_paths("solutions/{submission_id}/**");
    rules_.mutable_harness()->set_api_dep("//problem/harness:api");
    rules_.mutable_harness()->set_main_src("//problem/harness:main.cc");
    old_ = std::make_unique<CandidateStore>(dir_ / "old", CandidateLimits{},
                                            rules_);
  }
  void TearDown() override { std::filesystem::remove_all(dir_); }

  // A job for |participant|'s submission of |content|, with or without games.
  void Submit(const std::string &participant, const std::string &content,
              bool played) {
    proto::SubmitRequest request;
    request.set_author(participant);
    request.set_display_name(participant);
    request.set_game("nim");
    request.set_entry_header("strategy.h");
    auto *file = request.add_files();
    file->set_path("strategy.h");
    file->set_content(content);
    std::string error;
    const auto candidate = old_->Create(request, &error);
    ASSERT_TRUE(candidate.has_value()) << error;
    JobRecord record;
    record.mutable_job()->set_job_id("j" + std::to_string(++jobs_));
    record.mutable_job()->set_created_unix_ms(jobs_);
    *record.mutable_submission() = *candidate;
    JobRecord::Order *order = record.add_orders();
    if (played) {
      order->add_game_ids("g" + std::to_string(jobs_));
    }
    log_.Put(record);
    old_->SetStatus(candidate->candidate_id(), proto::Candidate::READY, "");
  }

  std::filesystem::path dir_;
  proto::SubmissionPolicy rules_;
  std::unique_ptr<CandidateStore> old_;
  JobLog log_{std::filesystem::temp_directory_path() /
              ("swiss_jobs_" + std::to_string(::getpid()))};
  int jobs_ = 0;
};

TEST_F(SeedVersionsTest, EveryPlayedVersionIsAnEntryInItsOwnDirectory) {
  Submit("alice", "// one\n", true);
  Submit("bob", "// bob\n", true);
  Submit("alice", "// one\n", true);      // the same code again
  Submit("alice", "// broken\n", false);  // never played
  Submit("alice", "// two\n", true);

  CandidateStore store(dir_ / "new", CandidateLimits{}, rules_);
  tournament_broker::GameHistory history(dir_ / "games");
  const TrueSkillStandings board(history, nullptr, {});
  const std::vector<SwissEntry> entries =
      SeedVersions(log_, "solutions", board, &store);
  std::filesystem::remove_all(std::filesystem::temp_directory_path() /
                              ("swiss_jobs_" + std::to_string(::getpid())));

  ASSERT_EQ(entries.size(), 3u);
  EXPECT_EQ(entries[0].id, "alice-v01");
  EXPECT_EQ(entries[1].id, "bob-v01");
  EXPECT_EQ(entries[2].id, "alice-v02");
  EXPECT_FALSE(entries[0].live);
  EXPECT_TRUE(entries[1].live && entries[2].live);
  EXPECT_EQ(entries[2].version, 2);
  EXPECT_GT(entries[1].board_rank, 0);

  const auto stored = store.Get("alice-v02");
  ASSERT_TRUE(stored.has_value());
  EXPECT_EQ(stored->status(), proto::Candidate::READY);
  EXPECT_NE(stored->patch().find("solutions/alice-v02/strategy.h"),
            std::string::npos);
  EXPECT_EQ(stored->patch().find("solutions/alice/"), std::string::npos);
  EXPECT_NE(stored->patch().find("// two"), std::string::npos);
}

}  // namespace
}  // namespace tournament_arena
