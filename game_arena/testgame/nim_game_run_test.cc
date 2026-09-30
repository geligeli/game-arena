// End-to-end cover for the arena's match loop, played on the arena's own game.
//
// The equivalent test against a real game lives downstream,
// which is the point: the broker core has to be provable without any game
// framework in the build, or the arena is not actually independent of one.

#include <unistd.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "game_arena/referee/game_registry.h"
#include "game_arena/referee/game_run.h"
#include "game_arena/referee/matchmaker.h"
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

  // A nonzero delay answers from another thread, so the referee clocks it.
  FakeClient(std::string name, Mode mode, milliseconds delay = milliseconds(0))
      : name_(std::move(name)), mode_(mode), delay_(delay) {}

  ~FakeClient() override {
    for (std::thread& t : threads_) {
      t.join();
    }
  }

  std::string name() const override { return name_; }

  bool Send(const proto::ServerMessage& msg) override {
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
    if (msg.has_game_start()) {
      std::lock_guard lock(mu_);
      seat_ = msg.game_start().seat();
      return true;
    }
    if (!msg.has_your_turn()) {
      return true;
    }
    switch (mode_) {
      case Mode::kSilent:
        break;
      case Mode::kIllegal:
        Deliver(std::string(kUnparseableAction));
        break;
      case Mode::kPlayValid:
        if (delay_.count() == 0) {
          Deliver(std::string(kTakeOne));
          break;
        }
        threads_.emplace_back([this] {
          std::this_thread::sleep_for(delay_);
          Deliver(std::string(kTakeOne));
        });
        break;
    }
    return true;
  }

  std::optional<std::string> TryPopAction() override {
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

  bool disconnected() const override {
    std::lock_guard lock(mu_);
    return disconnected_;
  }

  void CloseAfterFlush() override {
    std::lock_guard lock(mu_);
    closed_ = true;
  }

  std::optional<proto::GameOver> game_over() const {
    std::lock_guard lock(mu_);
    return game_over_;
  }

  bool closed() const {
    std::lock_guard lock(mu_);
    return closed_;
  }

  std::optional<int> seat() const {
    std::lock_guard lock(mu_);
    return seat_;
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
  const milliseconds delay_;
  std::vector<std::thread> threads_;  // touched only from Send, on the strand

  mutable std::mutex mu_;
  std::vector<std::string> inbox_;
  std::function<void()> observer_;
  std::optional<proto::GameOver> game_over_;
  std::optional<int> seat_;
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
    history_ = std::make_unique<GameHistory>(dir_ / "games");
    pool_ = std::make_unique<WorkerPool>(2);
    timer_ = std::make_unique<Timer>();
  }

  void TearDown() override {
    pool_->Stop();
    timer_->Stop();
    std::filesystem::remove_all(dir_);
  }

  static Seat MakeSeat(const std::shared_ptr<FakeClient>& client) {
    return Seat{
        .display_name = client->name(), .client = client, .builtin = nullptr};
  }

  static Seat MakeBuiltinSeat(const std::string& spec) {
    std::string error;
    auto builtin = GameRegistry().at("nim").make_builtin(spec, &error);
    EXPECT_TRUE(builtin.has_value()) << error;
    return Seat{.display_name = "builtin:" + spec,
                .client = nullptr,
                .builtin = std::move(*builtin)};
  }

  void RunToCompletion(std::vector<Seat> seats, GameRunConfig config) {
    std::promise<void> finished;
    auto done = finished.get_future();
    auto run = std::make_shared<GameRun>(
        GameRegistry().at(seats.size() == 2 ? "nim" : "nim3"), config,
        std::move(seats), ++counter_, history_.get(), pool_.get(), timer_.get(),
        [&finished] { finished.set_value(); });
    run->Start();
    ASSERT_EQ(done.wait_for(std::chrono::seconds(10)),
              std::future_status::ready)
        << "game never concluded";
  }

  std::filesystem::path dir_;
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

TEST_F(NimGameRunTest, RecordsAViewOfEveryState) {
  std::optional<proto::GameRecord> record;
  GameRunConfig config;
  config.on_record = [&record](const proto::GameRecord& r) { record = r; };
  auto alice =
      std::make_shared<FakeClient>("alice", FakeClient::Mode::kPlayValid);
  auto bob = std::make_shared<FakeClient>("bob", FakeClient::Mode::kPlayValid);
  RunToCompletion({MakeSeat(alice), MakeSeat(bob)}, config);

  ASSERT_TRUE(record.has_value());
  EXPECT_EQ(record->initial_view(), "21:0");
  ASSERT_EQ(record->steps_size(), 21);
  EXPECT_EQ(record->steps(0).view(), "20:1");
  EXPECT_EQ(record->steps(0).caption(), "seat 0 takes 1");
  EXPECT_EQ(record->steps(20).view(), "0:0");
}

TEST_F(NimGameRunTest, DrawGoesToTheFasterSeat) {
  auto fast =
      std::make_shared<FakeClient>("fast", FakeClient::Mode::kPlayValid);
  auto slow = std::make_shared<FakeClient>("slow", FakeClient::Mode::kPlayValid,
                                           milliseconds(20));
  GameRunConfig config;
  config.max_moves_per_game = 4;  // Nim never draws on its own
  RunToCompletion({MakeSeat(slow), MakeSeat(fast)}, config);

  ASSERT_TRUE(fast->game_over().has_value());
  EXPECT_EQ(fast->game_over()->result(), proto::GameOver::WIN);
  EXPECT_EQ(fast->game_over()->reason(), "time_tiebreak");
  ASSERT_TRUE(slow->game_over().has_value());
  EXPECT_EQ(slow->game_over()->result(), proto::GameOver::LOSS);
}

// With three seats a forfeiter drops to last and a builtin plays on for it, so
// the other two are still ranked by the game; it hears so when the game ends.
TEST_F(NimGameRunTest, ThreeSeatForfeitPlaysOn) {
  std::optional<proto::GameRecord> record;
  GameRunConfig config;
  config.turn_timeout = milliseconds(50);
  config.on_record = [&record](const proto::GameRecord& r) { record = r; };
  auto quiet = std::make_shared<FakeClient>("quiet", FakeClient::Mode::kSilent);
  auto alice =
      std::make_shared<FakeClient>("alice", FakeClient::Mode::kPlayValid);
  auto bob = std::make_shared<FakeClient>("bob", FakeClient::Mode::kPlayValid);
  RunToCompletion({MakeSeat(quiet), MakeSeat(alice), MakeSeat(bob)}, config);

  ASSERT_TRUE(record.has_value());
  EXPECT_EQ(record->termination_reason(), "normal");
  ASSERT_EQ(record->forfeits_size(), 1);
  EXPECT_EQ(record->forfeits(0).seat(), 0);
  EXPECT_EQ(record->forfeits(0).reason(), "timeout");
  EXPECT_EQ(record->forfeits(0).move(), 0);
  EXPECT_EQ(record->places(0), 2);
  EXPECT_EQ(record->places(1) + record->places(2), 1);
  ASSERT_TRUE(quiet->game_over().has_value());
  EXPECT_EQ(quiet->game_over()->reason(), "timeout");
  EXPECT_EQ(quiet->game_over()->place(), 2);
  EXPECT_EQ(quiet->game_over()->result(), proto::GameOver::LOSS);
  ASSERT_TRUE(alice->game_over().has_value());
  EXPECT_EQ(alice->game_over()->reason(), "normal");
}

TEST_F(NimGameRunTest, ThreeSeatsLevelAtTheMoveCapGoByThinkingTime) {
  auto fast =
      std::make_shared<FakeClient>("fast", FakeClient::Mode::kPlayValid);
  auto mid = std::make_shared<FakeClient>("mid", FakeClient::Mode::kPlayValid,
                                          milliseconds(10));
  auto slow = std::make_shared<FakeClient>("slow", FakeClient::Mode::kPlayValid,
                                           milliseconds(30));
  GameRunConfig config;
  config.max_moves_per_game = 6;
  RunToCompletion({MakeSeat(slow), MakeSeat(fast), MakeSeat(mid)}, config);

  ASSERT_TRUE(fast->game_over().has_value() && mid->game_over().has_value() &&
              slow->game_over().has_value());
  EXPECT_EQ(fast->game_over()->place(), 0);
  EXPECT_EQ(fast->game_over()->result(), proto::GameOver::WIN);
  EXPECT_EQ(fast->game_over()->reason(), "time_tiebreak");
  EXPECT_EQ(mid->game_over()->place(), 1);
  EXPECT_EQ(slow->game_over()->place(), 2);
}

// Two forfeits of three: the one left wins at once, the first to go is last.
TEST_F(NimGameRunTest, LastSeatStandingWins) {
  auto quiet = std::make_shared<FakeClient>("quiet", FakeClient::Mode::kSilent);
  auto cheat =
      std::make_shared<FakeClient>("cheat", FakeClient::Mode::kIllegal);
  auto honest =
      std::make_shared<FakeClient>("honest", FakeClient::Mode::kPlayValid);
  GameRunConfig config;
  config.turn_timeout = milliseconds(50);
  RunToCompletion({MakeSeat(cheat), MakeSeat(quiet), MakeSeat(honest)}, config);

  ASSERT_TRUE(honest->game_over().has_value());
  EXPECT_EQ(honest->game_over()->place(), 0);
  EXPECT_EQ(honest->game_over()->result(), proto::GameOver::WIN);
  EXPECT_EQ(quiet->game_over()->place(), 1);
  EXPECT_EQ(quiet->game_over()->reason(), "timeout");
  EXPECT_EQ(cheat->game_over()->place(), 2);
  EXPECT_EQ(cheat->game_over()->reason(), "illegal_action");
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

// Seat 0 alternates across the games two sides play each other, and the
// same code decides it whether the other side is a builtin or a player.
class NimMatchmakerTest : public NimGameRunTest {
 protected:
  // One game per stream: a new connection for each.
  int PlayOne(Matchmaker& matchmaker, const std::string& name,
              const std::vector<std::string>& opponents) {
    auto client =
        std::make_shared<FakeClient>(name, FakeClient::Mode::kPlayValid);
    proto::Hello hello;
    hello.set_player_name(name);
    hello.set_game(opponents.size() == 1 ? "nim" : "nim3");
    for (const std::string& opponent : opponents) {
      hello.add_opponent(opponent);
    }
    std::string error;
    EXPECT_TRUE(matchmaker.Join(client, hello, &error)) << error;
    matchmaker.Drain();
    EXPECT_TRUE(client->game_over().has_value());
    return client->seat().value_or(-1);
  }
};

TEST_F(NimMatchmakerTest, SeatsAlternateAgainstABuiltin) {
  Matchmaker matchmaker(MatchmakerConfig{}, history_.get());
  std::vector<int> seats;
  for (int game = 0; game < 4; ++game) {
    seats.push_back(PlayOne(matchmaker, "alice", {"builtin:random"}));
  }
  EXPECT_EQ(seats, (std::vector<int>{0, 1, 0, 1}));
  // Its own series: another builtin starts over.
  EXPECT_EQ(PlayOne(matchmaker, "alice", {"builtin:optimal"}), 0);
}

TEST_F(NimMatchmakerTest, SeatsAlternateBetweenPlayersWhoeverArrivesFirst) {
  Matchmaker matchmaker(MatchmakerConfig{}, history_.get());
  std::vector<int> alice_seats;
  for (int game = 0; game < 4; ++game) {
    // The side that parks first alternates too, so it is not what decides.
    const bool alice_first = game % 3 == 0;
    auto alice =
        std::make_shared<FakeClient>("alice", FakeClient::Mode::kPlayValid);
    auto bob =
        std::make_shared<FakeClient>("bob", FakeClient::Mode::kPlayValid);
    std::string error;
    for (const auto& [client, partner] :
         alice_first ? std::array{std::pair{alice, "player:bob"},
                                  std::pair{bob, "player:alice"}}
                     : std::array{std::pair{bob, "player:alice"},
                                  std::pair{alice, "player:bob"}}) {
      proto::Hello hello;
      hello.set_player_name(client->name());
      hello.set_game("nim");
      hello.add_opponent(partner);
      ASSERT_TRUE(matchmaker.Join(client, hello, &error)) << error;
    }
    matchmaker.Drain();
    ASSERT_TRUE(alice->seat().has_value() && bob->seat().has_value());
    EXPECT_NE(*alice->seat(), *bob->seat());
    alice_seats.push_back(*alice->seat());
  }
  EXPECT_EQ(alice_seats, (std::vector<int>{0, 1, 0, 1}));
}

TEST_F(NimMatchmakerTest, TwoBuiltinsPlayWithNoClientAndAlternateSeats) {
  std::vector<proto::GameRecord> records;
  MatchmakerConfig config;
  config.on_record = [&records](const proto::GameRecord& r) {
    records.push_back(r);
  };
  Matchmaker matchmaker(config, history_.get());
  std::string error;
  for (int game = 0; game < 2; ++game) {
    ASSERT_TRUE(matchmaker.StartBuiltins(
        "nim", {"builtin:random", "builtin:optimal"}, &error))
        << error;
    matchmaker.Drain();
  }
  ASSERT_EQ(records.size(), 2u);
  EXPECT_EQ(records[0].player_names(0), records[1].player_names(1));
  EXPECT_FALSE(matchmaker.StartBuiltins(
      "nim", {"builtin:random", "builtin:nonsense"}, &error));
}

// Every three games each member has each seat once, and the next three take
// the other order of the rest.
TEST_F(NimMatchmakerTest, ThreeSeatsRotateThroughEveryOrder) {
  Matchmaker matchmaker(MatchmakerConfig{}, history_.get());
  std::vector<int> seats;
  for (int game = 0; game < 6; ++game) {
    seats.push_back(
        PlayOne(matchmaker, "alice", {"builtin:random", "builtin:optimal"}));
  }
  EXPECT_EQ(seats, (std::vector<int>{0, 2, 1, 0, 2, 1}));
}

TEST_F(NimMatchmakerTest, ThreePlayersMeetWhoeverArrivesFirst) {
  Matchmaker matchmaker(MatchmakerConfig{}, history_.get());
  const std::vector<std::string> names = {"carol", "alice", "bob"};
  std::vector<std::shared_ptr<FakeClient>> clients;
  std::string error;
  for (const std::string& name : names) {
    clients.push_back(
        std::make_shared<FakeClient>(name, FakeClient::Mode::kPlayValid));
    proto::Hello hello;
    hello.set_player_name(name);
    hello.set_game("nim3");
    for (const std::string& other : names) {
      if (other != name) {
        hello.add_opponent("player:" + other);
      }
    }
    ASSERT_TRUE(matchmaker.Join(clients.back(), hello, &error)) << error;
  }
  matchmaker.Drain();
  std::vector<int> seats;
  for (const auto& client : clients) {
    ASSERT_TRUE(client->game_over().has_value());
    seats.push_back(client->seat().value_or(-1));
  }
  std::ranges::sort(seats);
  EXPECT_EQ(seats, (std::vector<int>{0, 1, 2}));
}

TEST_F(NimMatchmakerTest, RefusesTheWrongNumberOfOpponents) {
  Matchmaker matchmaker(MatchmakerConfig{}, history_.get());
  auto client =
      std::make_shared<FakeClient>("alice", FakeClient::Mode::kPlayValid);
  proto::Hello hello;
  hello.set_player_name("alice");
  hello.set_game("nim3");
  hello.add_opponent("builtin:random");
  std::string error;
  EXPECT_FALSE(matchmaker.Join(client, hello, &error));
  hello.add_opponent("player:alice");
  EXPECT_FALSE(matchmaker.Join(client, hello, &error));
}

}  // namespace
}  // namespace tournament_broker
