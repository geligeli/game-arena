// Termination-matrix tests for the event-driven game runner, driven by fake
// handles instead of real gRPC streams.
//
// Every one of these outcomes -- timeout, disconnect, illegal action, the move
// cap, the game time budget, an aborted server -- was previously reachable only
// by standing up a server and misbehaving over the network, which is why most
// of them had no coverage at all.

#include "cpp/tournament_server/game_run.h"

#include <array>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include <unistd.h>

#include "gtest/gtest.h"
#include "game_mcts/cpp/tictactoe/tictactoe.h"
#include "game_mcts/cpp/tictactoe/tictactoe_serialization.h"
#include "cpp/tournament_server/game_registry.h"

namespace tournament_broker {
namespace {

using std::chrono::milliseconds;
using tictactoe::TicTacToe;
using ttt_traits = mcts::GameSerializationTraits<TicTacToe>;

// Wire bytes that cannot parse as a TicTacToeAction: field number 0 is illegal
// on the wire, so ApplySerializedAction rejects before any game logic runs.
constexpr std::string_view kUnparseableAction = "\xff\xff\xff\xff";

auto ValidMoveFor(const std::string &state_bytes, std::mt19937 *gen)
    -> std::string {
  tictactoe::proto::TicTacToeState state_proto;
  EXPECT_TRUE(state_proto.ParseFromString(state_bytes));
  const TicTacToe state = ttt_traits::StateFromProto(state_proto);
  return ttt_traits::ActionToProto(state.sample_action(*gen))
      .SerializeAsString();
}

class FakeClient final : public ClientHandle {
 public:
  enum class Mode {
    kPlayValid,   // answers every turn with a legal move
    kSilent,      // never answers
    kIllegal,     // answers with bytes that do not parse
    kDropOnTurn,  // disconnects the moment it is asked to move
  };

  FakeClient(std::string name, Mode mode, milliseconds think = milliseconds(0))
      : name_(std::move(name)), mode_(mode), think_(think), gen_(12345) {}

  ~FakeClient() override {
    for (std::thread &responder : responders_) {
      if (responder.joinable()) {
        responder.join();
      }
    }
  }

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
      case Mode::kDropOnTurn:
        MarkDisconnected();
        break;
      case Mode::kIllegal:
        Answer(std::string(kUnparseableAction));
        break;
      case Mode::kPlayValid:
        Answer(ValidMoveFor(msg.your_turn().state(), &gen_));
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
  // Answers now, or after |think_| on a helper thread. The delayed form is what
  // makes the game time budget observable: GameRun charges a seat the real
  // wall-clock gap between YourTurn and its answer.
  void Answer(std::string action) {
    if (think_ == milliseconds(0)) {
      Deliver(std::move(action));
      return;
    }
    responders_.emplace_back(
        [this, action = std::move(action), delay = think_]() mutable {
          std::this_thread::sleep_for(delay);
          Deliver(std::move(action));
        });
  }

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
  const milliseconds think_;

  mutable std::mutex mu_;
  std::mt19937 gen_;
  std::vector<std::string> inbox_;
  std::function<void()> observer_;
  std::optional<proto::GameOver> game_over_;
  bool disconnected_ = false;
  bool closed_ = false;
  std::vector<std::thread> responders_;
};

class GameRunTest : public ::testing::Test {
 protected:
  void SetUp() override {
    dir_ = std::filesystem::temp_directory_path() /
           ("game_run_" + std::to_string(::getpid()) + "_" +
            std::to_string(
                ::testing::UnitTest::GetInstance()->random_seed()));
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

  auto MakeSeat(const std::shared_ptr<FakeClient> &client) -> Seat {
    return Seat{.display_name = client->name(),
                .client = client,
                .builtin = nullptr};
  }

  auto MakeBuiltinSeat(const std::string &spec) -> Seat {
    std::string error;
    auto builtin = GameRegistry().at("tictactoe").make_builtin(spec, &error);
    EXPECT_TRUE(builtin.has_value()) << error;
    return Seat{.display_name = "builtin:" + spec,
                .client = nullptr,
                .builtin = std::move(*builtin)};
  }

  // Starts the game and blocks until it concludes. Fails rather than hangs if
  // the state machine ever stalls.
  void RunToCompletion(std::array<Seat, 2> seats, GameRunConfig config) {
    std::promise<void> finished;
    auto done = finished.get_future();
    auto run = GameRun::Create(GameRegistry().at("tictactoe"), config,
                               std::move(seats), ++counter_, elo_.get(),
                               history_.get(), pool_.get(), timer_.get(),
                               [&finished] { finished.set_value(); });
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

TEST_F(GameRunTest, TwoPlayingClientsFinishNormally) {
  auto alice = std::make_shared<FakeClient>("alice", FakeClient::Mode::kPlayValid);
  auto bob = std::make_shared<FakeClient>("bob", FakeClient::Mode::kPlayValid);
  RunToCompletion({MakeSeat(alice), MakeSeat(bob)}, GameRunConfig{});

  ASSERT_TRUE(alice->game_over().has_value());
  ASSERT_TRUE(bob->game_over().has_value());
  EXPECT_EQ(alice->game_over()->reason(), "normal");
  EXPECT_EQ(bob->game_over()->reason(), "normal");
  // Both streams are closed by the server, not left for the client to end.
  EXPECT_TRUE(alice->closed());
  EXPECT_TRUE(bob->closed());
  // And the result is persisted.
  EXPECT_EQ(history_->RecentGames(10).size(), 1u);
}

TEST_F(GameRunTest, SilentClientLosesOnTurnTimeout) {
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

TEST_F(GameRunTest, DisconnectHandsTheWinToTheOpponent) {
  auto quitter =
      std::make_shared<FakeClient>("quitter", FakeClient::Mode::kDropOnTurn);
  auto survivor =
      std::make_shared<FakeClient>("survivor", FakeClient::Mode::kPlayValid);
  GameRunConfig config;
  config.turn_timeout = milliseconds(2000);  // must not be what ends the game
  RunToCompletion({MakeSeat(quitter), MakeSeat(survivor)}, config);

  ASSERT_TRUE(survivor->game_over().has_value());
  EXPECT_EQ(survivor->game_over()->reason(), "opponent_disconnect");
  EXPECT_EQ(survivor->game_over()->result(), proto::GameOver::WIN);
}

TEST_F(GameRunTest, UnparseableActionLosesImmediately) {
  auto cheat =
      std::make_shared<FakeClient>("cheat", FakeClient::Mode::kIllegal);
  auto honest =
      std::make_shared<FakeClient>("honest", FakeClient::Mode::kPlayValid);
  GameRunConfig config;
  config.turn_timeout = milliseconds(2000);
  RunToCompletion({MakeSeat(cheat), MakeSeat(honest)}, config);

  ASSERT_TRUE(cheat->game_over().has_value());
  EXPECT_EQ(cheat->game_over()->reason(), "illegal_action");
  EXPECT_EQ(cheat->game_over()->result(), proto::GameOver::LOSS);
}

TEST_F(GameRunTest, MoveCapEndsTheGameAsADraw) {
  auto alice = std::make_shared<FakeClient>("alice", FakeClient::Mode::kPlayValid);
  auto bob = std::make_shared<FakeClient>("bob", FakeClient::Mode::kPlayValid);
  GameRunConfig config;
  config.max_moves_per_game = 3;
  RunToCompletion({MakeSeat(alice), MakeSeat(bob)}, config);

  ASSERT_TRUE(alice->game_over().has_value());
  EXPECT_EQ(alice->game_over()->reason(), "max_moves");
  EXPECT_EQ(alice->game_over()->result(), proto::GameOver::DRAW);
}

// The per-turn timeout bounds one move; only the budget bounds the whole game.
// A client answering comfortably inside turn_timeout every time still runs out.
TEST_F(GameRunTest, GameTimeBudgetEndsAStallingButPunctualClient) {
  auto slow = std::make_shared<FakeClient>("slow", FakeClient::Mode::kPlayValid,
                                           /*think=*/milliseconds(40));
  auto quick =
      std::make_shared<FakeClient>("quick", FakeClient::Mode::kPlayValid);
  GameRunConfig config;
  config.turn_timeout = milliseconds(2000);   // never the binding constraint
  config.game_time_budget = milliseconds(60);  // ~2 turns of thinking
  RunToCompletion({MakeSeat(slow), MakeSeat(quick)}, config);

  ASSERT_TRUE(slow->game_over().has_value());
  EXPECT_EQ(slow->game_over()->reason(), "time_budget");
  EXPECT_EQ(slow->game_over()->result(), proto::GameOver::LOSS);
}

TEST_F(GameRunTest, AbortConcludesAGameWaitingOnAClient) {
  auto quiet = std::make_shared<FakeClient>("quiet", FakeClient::Mode::kSilent);
  auto other = std::make_shared<FakeClient>("other", FakeClient::Mode::kSilent);

  GameRunConfig config;
  config.turn_timeout = milliseconds(60000);  // would otherwise park for a minute
  std::promise<void> finished;
  auto done = finished.get_future();
  auto run = GameRun::Create(GameRegistry().at("tictactoe"), config,
                             {MakeSeat(quiet), MakeSeat(other)}, ++counter_,
                             elo_.get(), history_.get(), pool_.get(),
                             timer_.get(), [&finished] { finished.set_value(); });
  run->Start();
  // Let it reach the point of waiting on a move.
  std::this_thread::sleep_for(milliseconds(50));
  run->Abort("server_shutdown");

  ASSERT_EQ(done.wait_for(std::chrono::seconds(5)), std::future_status::ready)
      << "Abort must not wait out the turn timeout";
  ASSERT_TRUE(quiet->game_over().has_value());
  EXPECT_EQ(quiet->game_over()->reason(), "server_shutdown");
}

TEST_F(GameRunTest, BuiltinSeatPlaysWithoutAClient) {
  auto human = std::make_shared<FakeClient>("human", FakeClient::Mode::kPlayValid);
  RunToCompletion({MakeSeat(human), MakeBuiltinSeat("minimax")},
                  GameRunConfig{});

  ASSERT_TRUE(human->game_over().has_value());
  EXPECT_EQ(human->game_over()->reason(), "normal");
  // Minimax is optimal at TicTacToe: a random opponent cannot beat it.
  EXPECT_NE(human->game_over()->result(), proto::GameOver::WIN);
}

}  // namespace
}  // namespace tournament_broker
