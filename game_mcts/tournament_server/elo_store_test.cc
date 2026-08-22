#include "cpp/tournament_server/elo_store.h"

#include <cmath>

#include "gtest/gtest.h"

namespace tournament_broker {
namespace {

class EloStoreTest : public ::testing::Test {
 protected:
  void SetUp() override {
    dir_ = std::filesystem::temp_directory_path() /
           ("elo_store_test_" + std::to_string(
                ::testing::UnitTest::GetInstance()->random_seed()));
    std::filesystem::remove_all(dir_);
    std::filesystem::create_directories(dir_);
  }
  void TearDown() override { std::filesystem::remove_all(dir_); }

  std::filesystem::path dir_;
};

TEST_F(EloStoreTest, DefaultRatingIsInitial) {
  EloStore store(dir_ / "ratings.pb", /*k_factor=*/32.0);
  EXPECT_DOUBLE_EQ(store.Get("tictactoe", "alice").elo(), 1500.0);
}

TEST_F(EloStoreTest, WinBetweenEqualsMovesUpByHalfK) {
  EloStore store(dir_ / "ratings.pb", /*k_factor=*/32.0);
  const auto [a, b] = store.RecordResult("tictactoe", "alice", "bob", 1.0);
  EXPECT_NEAR(a, 1516.0, 1e-9);
  EXPECT_NEAR(b, 1484.0, 1e-9);
  EXPECT_EQ(store.Get("tictactoe", "alice").wins(), 1);
  EXPECT_EQ(store.Get("tictactoe", "bob").losses(), 1);
}

TEST_F(EloStoreTest, DrawBetweenEqualsLeavesRatings) {
  EloStore store(dir_ / "ratings.pb", /*k_factor=*/32.0);
  const auto [a, b] = store.RecordResult("tictactoe", "alice", "bob", 0.5);
  EXPECT_NEAR(a, 1500.0, 1e-9);
  EXPECT_NEAR(b, 1500.0, 1e-9);
  EXPECT_EQ(store.Get("tictactoe", "alice").draws(), 1);
}

TEST_F(EloStoreTest, RatingsArePerGame) {
  EloStore store(dir_ / "ratings.pb", /*k_factor=*/32.0);
  store.RecordResult("tictactoe", "alice", "bob", 1.0);
  EXPECT_DOUBLE_EQ(store.Get("risk2", "alice").elo(), 1500.0);
  EXPECT_NEAR(store.Get("tictactoe", "alice").elo(), 1516.0, 1e-9);
}

TEST_F(EloStoreTest, PersistsAcrossReload) {
  const auto path = dir_ / "ratings.pb";
  {
    EloStore store(path, /*k_factor=*/32.0);
    store.RecordResult("tictactoe", "alice", "bob", 1.0);
    store.RecordResult("tictactoe", "alice", "carol", 0.0);
  }
  EloStore reloaded(path, /*k_factor=*/32.0);
  reloaded.Load();
  const auto rating = reloaded.Get("tictactoe", "alice");
  // Alice: +16 for beating Bob (equal ratings), then about -16.74 for losing
  // to Carol while rated 1516 vs 1500 (E = 1/(1+10^(-16/400)) ~ 0.523).
  EXPECT_NEAR(rating.elo(), 1516.0 - 16.74, 0.1);
  EXPECT_EQ(rating.wins(), 1);
  EXPECT_EQ(rating.losses(), 1);
  EXPECT_EQ(reloaded.Get("tictactoe", "bob").losses(), 1);
}

}  // namespace
}  // namespace tournament_broker
