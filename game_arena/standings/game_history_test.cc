#include "game_arena/standings/game_history.h"

#include <boost/json/parse.hpp>
#include <boost/json/value.hpp>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace tournament_broker {
namespace {

namespace json = boost::json;

using ::testing::ElementsAre;

proto::GameRecord MakeRecord(const std::string &game_id) {
  proto::GameRecord record;
  record.set_game_id(game_id);
  record.set_game("nim");
  record.add_player_names("alice");
  record.add_player_names("bob");
  record.set_result(proto::GameRecord::WIN);
  record.set_winning_player(1);
  record.set_termination_reason("normal");
  record.add_steps();
  record.add_steps();
  record.add_steps();
  record.set_finished_unix_ms(1700000000123);
  return record;
}

class GameHistoryTest : public ::testing::Test {
 protected:
  void SetUp() override {
    dir_ = std::filesystem::temp_directory_path() /
           ("game_history_test_" +
            std::to_string(::testing::UnitTest::GetInstance()->random_seed()));
    std::filesystem::remove_all(dir_);
    std::filesystem::create_directories(dir_);
  }

  void TearDown() override { std::filesystem::remove_all(dir_); }

  std::vector<std::string> IndexLines() const {
    std::vector<std::string> lines;
    std::ifstream in(dir_ / "index.jsonl");
    std::string line;
    while (std::getline(in, line)) {
      lines.push_back(line);
    }
    return lines;
  }

  std::filesystem::path dir_;
};

// The index is read by the leaderboard's /api/games and is on disk across
// restarts, so its exact shape is part of the contract rather than an
// implementation detail. Pinned deliberately: a renamed or reordered key here
// is a format change, not a refactor.
TEST_F(GameHistoryTest, IndexLineHasTheDocumentedShape) {
  GameHistory history(dir_);
  ASSERT_FALSE(history.Store(MakeRecord("g1")).empty());

  ASSERT_THAT(
      IndexLines(),
      ElementsAre(
          R"({"game_id":"g1","game":"nim","player0":"alice","player1":"bob",)"
          R"("result":2,"winning_player":1,"reason":"normal","moves":3,)"
          R"("finished_unix_ms":1700000000123})"));
}

TEST_F(GameHistoryTest, PlayerNameWithQuotesStaysOnOneLine) {
  proto::GameRecord record = MakeRecord("g2");
  record.set_player_names(0, "she said \"hi\"\nthen left\\");

  GameHistory history(dir_);
  ASSERT_FALSE(history.Store(record).empty());

  // The newline in the name must not split the record: one line in, one line
  // out, or every later reader of index.jsonl desynchronizes.
  const std::vector<std::string> lines = IndexLines();
  ASSERT_EQ(lines.size(), 1u);

  boost::system::error_code ec;
  const json::value parsed = json::parse(lines[0], ec);
  ASSERT_FALSE(ec) << ec.message() << ": " << lines[0];
  EXPECT_EQ(parsed.as_object().at("player0").as_string(),
            "she said \"hi\"\nthen left\\");
}

TEST_F(GameHistoryTest, RecentGamesIsOldestFirstAndBounded) {
  GameHistory history(dir_);
  for (const char *id : {"g1", "g2", "g3"}) {
    ASSERT_FALSE(history.Store(MakeRecord(id)).empty());
  }

  EXPECT_TRUE(history.RecentGames(0).empty());
  EXPECT_EQ(history.RecentGames(100).size(), 3u);

  const std::vector<std::string> last_two = history.RecentGames(2);
  ASSERT_EQ(last_two.size(), 2u);
  EXPECT_THAT(last_two[0], ::testing::HasSubstr(R"("game_id":"g2")"));
  EXPECT_THAT(last_two[1], ::testing::HasSubstr(R"("game_id":"g3")"));
}

TEST_F(GameHistoryTest, SeedsFromTheIndexOfAPreviousRun) {
  {
    GameHistory history(dir_);
    ASSERT_FALSE(history.Store(MakeRecord("g1")).empty());
    ASSERT_FALSE(history.Store(MakeRecord("g2")).empty());
  }

  // A fresh process over the same directory serves what the last one wrote.
  GameHistory reopened(dir_);
  EXPECT_EQ(reopened.RecentGames(100).size(), 2u);
  ASSERT_FALSE(reopened.Store(MakeRecord("g3")).empty());
  EXPECT_EQ(reopened.RecentGames(100).size(), 3u);
  EXPECT_EQ(IndexLines().size(), 3u);
}

TEST_F(GameHistoryTest, AllGamesReadsPastTheInMemoryTail) {
  GameHistory history(dir_);
  const int count = static_cast<int>(GameHistory::kRecentCapacity) + 10;
  for (int i = 0; i < count; ++i) {
    ASSERT_FALSE(history.Store(MakeRecord("g" + std::to_string(i))).empty());
  }

  EXPECT_EQ(history.RecentGames(count).size(), GameHistory::kRecentCapacity);
  const std::vector<std::string> all = history.AllGames();
  ASSERT_EQ(all.size(), static_cast<std::size_t>(count));
  EXPECT_THAT(all[0], ::testing::HasSubstr(R"("game_id":"g0")"));
}

TEST_F(GameHistoryTest, LoadsARecordByIdAndNothingOutsideItsDirectory) {
  GameHistory history(dir_ / "games");
  ASSERT_FALSE(history.Store(MakeRecord("o1_1-g1_0")).empty());
  std::ofstream(dir_ / "x.pb") << "";

  EXPECT_EQ(history.Load("o1_1-g1_0")->player_names(1), "bob");
  EXPECT_FALSE(history.Load("../x").has_value());
  EXPECT_FALSE(history.Load("").has_value());
  EXPECT_FALSE(history.Load("missing").has_value());
}

TEST_F(GameHistoryTest, WritesTheRecordProtoBesideTheIndex) {
  GameHistory history(dir_);
  const std::filesystem::path path = history.Store(MakeRecord("g1"));
  ASSERT_EQ(path, dir_ / "g1.pb");

  std::ifstream in(path, std::ios::binary);
  proto::GameRecord read_back;
  ASSERT_TRUE(read_back.ParseFromIstream(&in));
  EXPECT_EQ(read_back.game_id(), "g1");
  EXPECT_EQ(read_back.player_names(1), "bob");
}

}  // namespace
}  // namespace tournament_broker
