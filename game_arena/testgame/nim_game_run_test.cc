// End-to-end cover for the arena's match loop, played on the arena's own game.
//
// The equivalent test against a real game lives downstream,
// which is the point: the broker core has to be provable without any game
// framework in the build, or the arena is not actually independent of one.

#include <unistd.h>

#include <array>
#include <chrono>
#include <filesystem>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "game_arena/referee/game_registry.h"
#include "game_arena/referee/game_run.h"
#include "game_arena/testgame/nim.h"
#include "gtest/gtest.h"

namespace tournament_broker {
namespace {

using std::chrono::milliseconds;

// Always legal while a stone remains, so a "playing" client needs no strategy.
constexpr std::string_view kTakeOne = "1";
// Nim actions are decimal integers; this parses as none of them.
constexpr std::string_view kUnparseableAction = "not-a-number";

class FakeClient final : public ClientHandle {
 public:
  enum class Mode { kPlayValid, kSilent, kIllegal };

  FakeClient(std::string name, Mode mode)
      : name_(std::move(name)), mode_(mode) {}

  auto name() const -> std::string override { return name_; }

  auto Send(const proto::ServerMessage &msg) -> bool override {
    {
      std::lock_guard lock(mu_);
      if (disconnected_) {
        return false;
      }
    }
    if (msg.has_game_over()) {
      std::lock_guard lock(mu_);
      game_over_ = msg.game_over();
      return true;
    }
    if (!msg.has_your_turn()) {
      return true;  // game_start
    }
    switch (mode_) {
      case Mode::kSilent:
        break;
      case Mode::kIllegal:
        Deliver(std::string(kUnparseableAction));
        break;
      case Mode::kPlayValid:
        Deliver(std::string(kTakeOne));
        break;
    }
    return true;
  }

  auto TryPopAction() -> std::optional<std::string> override {
    std::lock_guard lock(mu_);
    if (inbox_.empty()) {
      return std::nullopt;
    }
    std::string action = std::move(inbox_.front());
    inbox_.erase(inbox_.begin());
    return action;
  }

  void SetObserver(std::function<void()> on_event) override {
    std::lock_guard lock(mu_);
    observer_ = std::move(on_event);
  }

  void MarkDisconnected() override {
    {
      std::lock_guard lock(mu_);
      disconnected_ = true;
    }
    Notify();
  }

  auto disconnected() const -> bool override {
    std::lock_guard lock(mu_);
    return disconnected_;
  }

  void CloseAfterFlush() override {
    std::lock_guard lock(mu_);
    closed_ = true;
  }

  auto game_over() const -> std::optional<proto::GameOver> {
    std::lock_guard lock(mu_);
    return game_over_;
  }

  auto closed() const -> bool {
    std::lock_guard lock(mu_);
    return closed_;
  }

 private:
  void Deliver(std::string action) {
    {
      std::lock_guard lock(mu_);
      if (disconnected_) {
        return;
      }
      inbox_.push_back(std::move(action));
    }
    Notify();
  }

  void Notify() {
    std::function<void()> observer;
    {
      std::lock_guard lock(mu_);
      observer = observer_;
    }
    if (observer) {
      observer();
    }
  }

  const std::string name_;
  const Mode mode_;

  mutable std::mutex mu_;
  std::vector<std::string> inbox_;
  std::function<void()> observer_;
  std::optional<proto::GameOver> game_over_;
  bool disconnected_ = false;
  bool closed_ = false;
};

class NimGameRunTest : public ::testing::Test {
 protected:
  void SetUp() override {
    dir_ = std::filesystem::temp_directory_path() /
           ("nim_game_run_" + std::to_string(::getpid()));
    std::filesystem::remove_all(dir_);
    std::filesystem::create_directories(dir_);
    elo_ = std::make_unique<EloStore>(dir_ / "ratings.pb", /*k_factor=*/32.0);
    history_ = std::make_unique<GameHistory>(dir_ / "games");
    pool_ = std::make_unique<WorkerPool>(2);
    timer_ = std::make_unique<Timer>();
  }

  void TearDown() override {
    pool_->Stop();
    timer_->Stop();
    std::filesystem::remove_all(dir_);
  }

  static auto MakeSeat(const std::shared_ptr<FakeClient> &client) -> Seat {
    return Seat{
        .display_name = client->name(), .client = client, .builtin = nullptr};
  }

  static auto MakeBuiltinSeat(const std::string &spec) -> Seat {
    std::string error;
    auto builtin = GameRegistry().at("nim").make_builtin(spec, &error);
    EXPECT_TRUE(builtin.has_value()) << error;
    return Seat{.display_name = "builtin:" + spec,
                .client = nullptr,
                .builtin = std::move(*builtin)};
  }

  void RunToCompletion(std::array<Seat, 2> seats, GameRunConfig config) {
    std::promise<void> finished;
    auto done = finished.get_future();
    auto run =
        GameRun::Create(GameRegistry().at("nim"), config, std::move(seats),
                        ++counter_, elo_.get(), history_.get(), pool_.get(),
                        timer_.get(), [&finished] { finished.set_value(); });
    run->Start();
    ASSERT_EQ(done.wait_for(std::chrono::seconds(10)),
              std::future_status::ready)
        << "game never concluded";
  }

  std::filesystem::path dir_;
  std::unique_ptr<EloStore> elo_;
  std::unique_ptr<GameHistory> history_;
  std::unique_ptr<WorkerPool> pool_;
  std::unique_ptr<Timer> timer_;
  uint64_t counter_ = 0;
};

TEST_F(NimGameRunTest, TwoPlayingClientsFinishNormally) {
  auto alice =
      std::make_shared<FakeClient>("alice", FakeClient::Mode::kPlayValid);
  auto bob = std::make_shared<FakeClient>("bob", FakeClient::Mode::kPlayValid);
  RunToCompletion({MakeSeat(alice), MakeSeat(bob)}, GameRunConfig{});

  ASSERT_TRUE(alice->game_over().has_value());
  ASSERT_TRUE(bob->game_over().has_value());
  EXPECT_EQ(alice->game_over()->reason(), "normal");
  EXPECT_EQ(bob->game_over()->reason(), "normal");
  EXPECT_TRUE(alice->closed());
  EXPECT_TRUE(bob->closed());
  EXPECT_EQ(history_->RecentGames(10).size(), 1u);
}

TEST_F(NimGameRunTest, SilentClientLosesOnTurnTimeout) {
  auto quiet = std::make_shared<FakeClient>("quiet", FakeClient::Mode::kSilent);
  auto active =
      std::make_shared<FakeClient>("active", FakeClient::Mode::kPlayValid);
  GameRunConfig config;
  config.turn_timeout = milliseconds(50);
  RunToCompletion({MakeSeat(quiet), MakeSeat(active)}, config);

  ASSERT_TRUE(quiet->game_over().has_value());
  EXPECT_EQ(quiet->game_over()->reason(), "timeout");
  EXPECT_EQ(quiet->game_over()->result(), proto::GameOver::LOSS);
  ASSERT_TRUE(active->game_over().has_value());
  EXPECT_EQ(active->game_over()->result(), proto::GameOver::WIN);
}

// The referee oracle is the arena's, not the game's: bytes that do not parse
// have to be refused by GameSession before any rules run.
TEST_F(NimGameRunTest, IllegalActionLosesTheGame) {
  auto cheat =
      std::make_shared<FakeClient>("cheat", FakeClient::Mode::kIllegal);
  auto honest =
      std::make_shared<FakeClient>("honest", FakeClient::Mode::kPlayValid);
  RunToCompletion({MakeSeat(cheat), MakeSeat(honest)}, GameRunConfig{});

  ASSERT_TRUE(cheat->game_over().has_value());
  EXPECT_EQ(cheat->game_over()->result(), proto::GameOver::LOSS);
  ASSERT_TRUE(honest->game_over().has_value());
  EXPECT_EQ(honest->game_over()->result(), proto::GameOver::WIN);
}

// A builtin seat takes the same path as a remote one, so the registry's
// make_builtin has to work through GameRun as well as in isolation.
TEST_F(NimGameRunTest, BuiltinSeatPlaysAGameThrough) {
  auto human =
      std::make_shared<FakeClient>("human", FakeClient::Mode::kPlayValid);
  RunToCompletion({MakeSeat(human), MakeBuiltinSeat("random")},
                  GameRunConfig{});

  ASSERT_TRUE(human->game_over().has_value());
  EXPECT_EQ(human->game_over()->reason(), "normal");
  EXPECT_EQ(history_->RecentGames(10).size(), 1u);
}

}  // namespace
}  // namespace tournament_broker
