// End-to-end test: in-process gRPC server + real client connections over
// loopback. Covers matchmaking (two clients, and client vs built-in), the
// turn time limit, game-history persistence, ELO updates, and the HTTP
// leaderboard.

#include <chrono>
#include <filesystem>
#include <future>
#include <memory>
#include <random>
#include <string>
#include <thread>

#include <grpcpp/grpcpp.h>

#include "gtest/gtest.h"
#include "game_mcts/cpp/tictactoe/tictactoe.h"
#include "game_mcts/cpp/tictactoe/tictactoe_serialization.h"
#include "game_mcts/tournament_server/broker_service.h"
#include "game_mcts/tournament_server/elo_store.h"
#include "game_mcts/tournament_server/game_history.h"
#include "game_mcts/tournament_server/http_leaderboard.h"
#include "game_mcts/tournament_server/matchmaker.h"
#include "game_mcts/tournament_server/remote_client.h"
#include "game_mcts/tournament_server/tournament_broker.grpc.pb.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace tournament_broker {
namespace {

using tictactoe::TicTacToe;
using ttt_traits = mcts::GameSerializationTraits<TicTacToe>;

struct ClientResult {
  bool rpc_ok = false;
  bool got_game_over = false;
  proto::GameOver::Result result = proto::GameOver::DRAW;
  std::string reason;
  std::string opponent_name;
  int seat = -1;
  double new_elo = 0.0;
};

class BrokerIntegrationTest : public ::testing::Test {
 protected:
  // Overridden by the fixtures below that need a different clock or a
  // rendezvous deadline; SetUp() calls it before starting the server.
  virtual auto MakeConfig() -> MatchmakerConfig {
    MatchmakerConfig config;
    config.turn_timeout = std::chrono::milliseconds(300);
    config.max_moves_per_game = 1000;
    return config;
  }

  // Long enough that no ordinary test trips it; the hello-deadline fixture
  // below shortens it.
  virtual auto HelloTimeout() -> std::chrono::milliseconds {
    return std::chrono::milliseconds(30000);
  }

  void SetUp() override {
    // Include the pid: the gtest seed alone collides when the same test binary
    // runs concurrently (bazel --runs_per_test).
    dir_ = std::filesystem::temp_directory_path() /
           ("broker_it_" + std::to_string(::getpid()) + "_" +
            std::to_string(
                ::testing::UnitTest::GetInstance()->random_seed()));
    std::filesystem::remove_all(dir_);
    std::filesystem::create_directories(dir_);

    elo_store_ = std::make_unique<EloStore>(dir_ / "ratings.pb",
                                            /*k_factor=*/32.0);
    history_ = std::make_unique<GameHistory>(dir_ / "games");
    matchmaker_ = std::make_unique<Matchmaker>(MakeConfig(), elo_store_.get(),
                                               history_.get());
    service_ =
        std::make_unique<BrokerService>(matchmaker_.get(), HelloTimeout());

    grpc::ServerBuilder builder;
    builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(),
                             &port_);
    builder.RegisterService(service_.get());
    server_ = builder.BuildAndStart();
    ASSERT_NE(server_, nullptr);
    ASSERT_GT(port_, 0);
  }

  void TearDown() override {
    // Deadline, not the no-arg form: Shutdown() waits indefinitely for
    // outstanding calls, and a client still queued for an opponent holds its
    // handler thread parked in a blocking Read forever.
    ShutdownServer();
    matchmaker_->Drain();
    server_.reset();
    std::filesystem::remove_all(dir_);
  }

  // Plays random valid TicTacToe moves until game over.
  auto RunRandomClient(const std::string &name, const std::string &opponent,
                       std::mt19937 *gen) -> ClientResult {
    return RunClient(name, opponent, /*stall=*/false, gen);
  }

  // |stall|: send hello, then never answer a turn (exercises the timeout).
  // |think_time|: sleep before each reply (exercises the game time budget,
  // which no single turn is slow enough to trip on its own).
  auto RunClient(const std::string &name, const std::string &opponent,
                 bool stall, std::mt19937 *gen,
                 std::chrono::milliseconds think_time =
                     std::chrono::milliseconds(0)) -> ClientResult {
    ClientResult result;
    auto channel = grpc::CreateChannel("127.0.0.1:" + std::to_string(port_),
                                       grpc::InsecureChannelCredentials());
    auto stub = proto::TournamentBroker::NewStub(channel);
    grpc::ClientContext context;
    auto stream = stub->Play(&context);

    proto::ClientMessage hello_msg;
    auto *hello = hello_msg.mutable_hello();
    hello->set_player_name(name);
    hello->set_game("tictactoe");
    hello->set_opponent(opponent);
    if (!stream->Write(hello_msg)) {
      return result;
    }

    // A write failing means the server already finished the call -- typically
    // because this client just ran out of time. Reading must continue anyway:
    // Finish() blocks until the stream is drained, so bailing out here with a
    // GameOver still queued would hang instead of reporting the loss.
    bool writes_failed = false;
    proto::ServerMessage msg;
    while (stream->Read(&msg)) {
      if (msg.has_your_turn() && !stall && !writes_failed) {
        if (think_time.count() > 0) {
          std::this_thread::sleep_for(think_time);
        }
        tictactoe::proto::TicTacToeState state_proto;
        EXPECT_TRUE(state_proto.ParseFromString(msg.your_turn().state()));
        const TicTacToe state = ttt_traits::StateFromProto(state_proto);
        proto::ClientMessage reply;
        reply.mutable_action()->set_action(
            ttt_traits::ActionToProto(state.sample_action(*gen))
                .SerializeAsString());
        writes_failed = !stream->Write(reply);
      } else if (msg.has_game_start()) {
        result.opponent_name = msg.game_start().opponent_name();
        result.seat = msg.game_start().seat();
      } else if (msg.has_game_over()) {
        result.got_game_over = true;
        result.result = msg.game_over().result();
        result.reason = msg.game_over().reason();
        result.new_elo = msg.game_over().new_elo();
        break;  // One game per stream; half-close so the server exits.
      }
    }
    stream->WritesDone();
    result.rpc_ok = stream->Finish().ok();
    return result;
  }

  // Sends hello, waits for its first turn, then abandons the RPC mid-game.
  // Drives the reactor's cancel path while the game thread may be mid-write --
  // the ordering that used to destroy the stream under the game thread's feet.
  void RunAbandoningClient(const std::string &name) {
    auto channel = grpc::CreateChannel("127.0.0.1:" + std::to_string(port_),
                                       grpc::InsecureChannelCredentials());
    auto stub = proto::TournamentBroker::NewStub(channel);
    grpc::ClientContext context;
    auto stream = stub->Play(&context);

    proto::ClientMessage hello_msg;
    auto *hello = hello_msg.mutable_hello();
    hello->set_player_name(name);
    hello->set_game("tictactoe");
    hello->set_opponent("any");
    if (!stream->Write(hello_msg)) {
      return;
    }
    proto::ServerMessage msg;
    while (stream->Read(&msg)) {
      if (msg.has_your_turn()) {
        context.TryCancel();  // rage-quit without answering
        break;
      }
    }
    stream->Finish().ok();  // status is CANCELLED; we do not care
  }

  // Sends only a hello and reports how the server answered. Used for the
  // joins the broker must refuse outright.
  auto HelloStatus(const std::string &name, const std::string &opponent)
      -> grpc::Status {
    auto channel = grpc::CreateChannel("127.0.0.1:" + std::to_string(port_),
                                       grpc::InsecureChannelCredentials());
    auto stub = proto::TournamentBroker::NewStub(channel);
    grpc::ClientContext context;
    auto stream = stub->Play(&context);
    proto::ClientMessage hello_msg;
    auto *hello = hello_msg.mutable_hello();
    hello->set_player_name(name);
    hello->set_game("tictactoe");
    hello->set_opponent(opponent);
    EXPECT_TRUE(stream->Write(hello_msg));
    stream->WritesDone();
    return stream->Finish();
  }

  // Polls until |count| clients are parked for a named partner.
  void WaitForParked(int count) {
    for (int i = 0; i < 400 && matchmaker_->parked("tictactoe") != count; ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    ASSERT_EQ(matchmaker_->parked("tictactoe"), count);
  }

  // Cancels any still-pending Play calls. Idempotent.
  void ShutdownServer() {
    server_->Shutdown(std::chrono::system_clock::now() +
                      std::chrono::seconds(2));
  }

  // Polls until exactly |count| clients are parked in the tictactoe queue, so
  // tests can pin down join ordering without sleeping on a guess.
  void WaitForQueued(int count) {
    for (int i = 0; i < 400 && matchmaker_->queued("tictactoe") != count; ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    ASSERT_EQ(matchmaker_->queued("tictactoe"), count);
  }

  std::filesystem::path dir_;
  std::unique_ptr<EloStore> elo_store_;
  std::unique_ptr<GameHistory> history_;
  std::unique_ptr<Matchmaker> matchmaker_;
  std::unique_ptr<BrokerService> service_;
  std::unique_ptr<grpc::Server> server_;
  int port_ = 0;
};

TEST_F(BrokerIntegrationTest, TwoClientsPlayFullGame) {
  std::mt19937 gen_a(1), gen_b(2);
  auto future_a = std::async(std::launch::async, [&] {
    return RunRandomClient("alice", "any", &gen_a);
  });
  auto future_b = std::async(std::launch::async, [&] {
    return RunRandomClient("bob", "any", &gen_b);
  });
  const ClientResult a = future_a.get();
  const ClientResult b = future_b.get();

  ASSERT_TRUE(a.rpc_ok);
  ASSERT_TRUE(b.rpc_ok);
  EXPECT_EQ(a.reason, "normal");
  EXPECT_EQ(b.reason, "normal");
  // TicTacToe with random play ends in a win or a draw; the two clients must
  // agree on the outcome shape.
  if (a.result == proto::GameOver::WIN) {
    EXPECT_EQ(b.result, proto::GameOver::LOSS);
  } else if (a.result == proto::GameOver::LOSS) {
    EXPECT_EQ(b.result, proto::GameOver::WIN);
  } else {
    EXPECT_EQ(b.result, proto::GameOver::DRAW);
  }

  // ELO updated and persisted.
  const auto rating_a = elo_store_->Get("tictactoe", "alice");
  const auto rating_b = elo_store_->Get("tictactoe", "bob");
  EXPECT_EQ(rating_a.wins() + rating_a.draws() + rating_a.losses(), 1);
  EXPECT_EQ(rating_b.wins() + rating_b.draws() + rating_b.losses(), 1);
  EXPECT_NEAR(rating_a.elo() + rating_b.elo(), 3000.0, 1e-6);
  EXPECT_DOUBLE_EQ(a.new_elo, rating_a.elo());

  // History written: one .pb record plus the index line.
  EXPECT_EQ(history_->RecentGames(10).size(), 1);
  int pb_files = 0;
  for (const auto &entry : std::filesystem::directory_iterator(
           dir_ / "games")) {
    if (entry.path().extension() == ".pb") {
      ++pb_files;
    }
  }
  EXPECT_EQ(pb_files, 1);
}

// Regression: two clients sharing a name sit at the head of the queue. They
// can never play each other, but they must not stop a third, distinct player
// from being matched with one of them.
TEST_F(BrokerIntegrationTest, SameNamedClientsDoNotBlockMatchmaking) {
  std::mt19937 gen_a1(11), gen_a2(12), gen_bob(13);

  auto alice1 = std::async(std::launch::async, [&] {
    return RunRandomClient("alice", "any", &gen_a1);
  });
  WaitForQueued(1);
  auto alice2 = std::async(std::launch::async, [&] {
    return RunRandomClient("alice", "any", &gen_a2);
  });
  WaitForQueued(2);
  // Queue is now [alice, alice]; bob must still find a game.
  auto bob = std::async(std::launch::async, [&] {
    return RunRandomClient("bob", "any", &gen_bob);
  });

  const ClientResult bob_result = bob.get();
  const ClientResult alice1_result = alice1.get();
  EXPECT_TRUE(bob_result.got_game_over);
  EXPECT_EQ(bob_result.reason, "normal");
  EXPECT_EQ(bob_result.opponent_name, "alice");
  EXPECT_TRUE(alice1_result.got_game_over);
  EXPECT_EQ(alice1_result.opponent_name, "bob");

  // The leftover "alice" has no legal opponent and stays queued; release it by
  // shutting the server down (TearDown's second Shutdown is a no-op).
  EXPECT_EQ(matchmaker_->queued("tictactoe"), 1);
  ShutdownServer();
  const ClientResult alice2_result = alice2.get();
  EXPECT_FALSE(alice2_result.got_game_over);
}

// A client that vanishes mid-game hands the win to its opponent. Under asan
// and tsan this is the regression test for the old raw-stream use-after-free:
// the RPC used to be torn down while the game thread was between its
// disconnected() check and stream_->Write().
TEST_F(BrokerIntegrationTest, ClientDisconnectsMidGame) {
  std::mt19937 gen(21);
  auto quitter =
      std::async(std::launch::async, [&] { RunAbandoningClient("quitter"); });
  WaitForQueued(1);
  auto survivor = std::async(std::launch::async, [&] {
    return RunRandomClient("survivor", "any", &gen);
  });

  quitter.get();
  const ClientResult result = survivor.get();
  ASSERT_TRUE(result.got_game_over);
  EXPECT_EQ(result.result, proto::GameOver::WIN);
  EXPECT_EQ(result.reason, "opponent_disconnect");
}

TEST_F(BrokerIntegrationTest, ClientVsBuiltinMinimaxNeverWins) {
  std::mt19937 gen(3);
  const ClientResult result = RunRandomClient("carol", "builtin:minimax", &gen);
  ASSERT_TRUE(result.rpc_ok);
  EXPECT_EQ(result.reason, "normal");
  // Minimax is optimal: the random client can at best draw.
  EXPECT_NE(result.result, proto::GameOver::WIN);
  EXPECT_LE(elo_store_->Get("tictactoe", "carol").elo(), 1500.0);
  // The built-in is rated too.
  EXPECT_GE(elo_store_->Get("tictactoe", "builtin:minimax").elo(), 1500.0);
}

TEST_F(BrokerIntegrationTest, TurnTimeoutLosesGame) {
  std::mt19937 gen(4);
  const ClientResult result =
      RunClient("slow-poke", "builtin:random", /*stall=*/true, &gen);
  ASSERT_TRUE(result.rpc_ok);
  EXPECT_EQ(result.result, proto::GameOver::LOSS);
  EXPECT_EQ(result.reason, "timeout");
  EXPECT_LT(result.new_elo, 1500.0);
  EXPECT_GT(elo_store_->Get("tictactoe", "builtin:random").elo(), 1500.0);
}

// The typed client wrapper: an MctsPolicy<TicTacToe> joins the broker over a
// real gRPC connection and plays against a built-in.
TEST_F(BrokerIntegrationTest, TypedPolicyClientVsBuiltin) {
  auto channel = grpc::CreateChannel("127.0.0.1:" + std::to_string(port_),
                                     grpc::InsecureChannelCredentials());
  auto stub = proto::TournamentBroker::NewStub(channel);

  using proposer_t = mcts::DefaultProposer<TicTacToe>;
  using rollout_t = mcts::RandomRollout<TicTacToe, proposer_t>;
  MctsPolicy<TicTacToe, proposer_t, rollout_t> policy{.iterations = 200};

  std::mt19937 gen(6);
  const auto results = PlayRemoteGames<TicTacToe>(
      stub.get(), "frank", "tictactoe", "builtin:minimax", /*num_games=*/1,
      policy, gen);
  ASSERT_EQ(results.size(), 1);
  EXPECT_EQ(results[0].reason, "normal");
  // Minimax is optimal; MCTS with random rollouts cannot beat it.
  EXPECT_NE(results[0].result, proto::GameOver::WIN);
  EXPECT_DOUBLE_EQ(results[0].new_elo,
                   elo_store_->Get("tictactoe", "frank").elo());

  // Two typed clients can also be paired through the "any" queue.
  ProposerPolicy<TicTacToe, proposer_t> random_policy{};
  auto future_g = std::async(std::launch::async, [&] {
    std::mt19937 gen_g(7);
    auto ch = grpc::CreateChannel("127.0.0.1:" + std::to_string(port_),
                                  grpc::InsecureChannelCredentials());
    auto st = proto::TournamentBroker::NewStub(ch);
    return PlayRemoteGames<TicTacToe>(st.get(), "grace", "tictactoe", "any",
                                      1, random_policy, gen_g);
  });
  std::mt19937 gen_h(8);
  const auto results_h = PlayRemoteGames<TicTacToe>(
      stub.get(), "heidi", "tictactoe", "any", 1, random_policy, gen_h);
  const auto results_g = future_g.get();
  ASSERT_EQ(results_g.size(), 1);
  ASSERT_EQ(results_h.size(), 1);
  EXPECT_EQ(results_g[0].reason, "normal");
  EXPECT_EQ(results_h[0].reason, "normal");
}

// --- opponent = "player:<name>" -------------------------------------------
//
// The FIFO "any" queue cannot express "these two specific candidates play each
// other", which is what the arena scheduler needs when it dispatches both
// sides of a match to two sandbox workers.

TEST_F(BrokerIntegrationTest, RendezvousPairsNamedPlayers) {
  std::mt19937 gen_a(31), gen_b(32);
  auto future_a = std::async(std::launch::async, [&] {
    return RunRandomClient("alice", "player:bob", &gen_a);
  });
  WaitForParked(1);
  // Alice is waiting for bob specifically, not sitting in the open queue.
  EXPECT_EQ(matchmaker_->queued("tictactoe"), 0);
  auto future_b = std::async(std::launch::async, [&] {
    return RunRandomClient("bob", "player:alice", &gen_b);
  });

  const ClientResult a = future_a.get();
  const ClientResult b = future_b.get();
  ASSERT_TRUE(a.got_game_over);
  ASSERT_TRUE(b.got_game_over);
  EXPECT_EQ(a.reason, "normal");
  EXPECT_EQ(b.reason, "normal");
  EXPECT_EQ(a.opponent_name, "bob");
  EXPECT_EQ(b.opponent_name, "alice");
  EXPECT_EQ(matchmaker_->parked("tictactoe"), 0);
}

// Who parks first is a race between two workers, so it must not decide who
// moves first: seats alternate across the pair's series instead.
TEST_F(BrokerIntegrationTest, RendezvousAlternatesSeatsAcrossASeries) {
  std::vector<int> alice_seats;
  for (int game = 0; game < 2; ++game) {
    std::mt19937 gen_a(41 + game), gen_b(51 + game);
    auto future_a = std::async(std::launch::async, [&] {
      return RunRandomClient("alice", "player:bob", &gen_a);
    });
    WaitForParked(1);
    auto future_b = std::async(std::launch::async, [&] {
      return RunRandomClient("bob", "player:alice", &gen_b);
    });
    const ClientResult a = future_a.get();
    const ClientResult b = future_b.get();
    ASSERT_TRUE(a.got_game_over);
    ASSERT_TRUE(b.got_game_over);
    ASSERT_NE(a.seat, b.seat);
    alice_seats.push_back(a.seat);
  }
  // Alice parked first in both games, yet took each seat once.
  EXPECT_NE(alice_seats[0], alice_seats[1]);
}

// A rendezvous must not be satisfied by whoever happens to be around.
TEST_F(BrokerIntegrationTest, RendezvousIgnoresUnrelatedPlayers) {
  std::mt19937 gen_a(33), gen_c(34);
  auto future_a = std::async(std::launch::async, [&] {
    return RunRandomClient("alice", "player:bob", &gen_a);
  });
  WaitForParked(1);
  auto future_c = std::async(std::launch::async, [&] {
    return RunRandomClient("carol", "any", &gen_c);
  });
  WaitForQueued(1);

  // Carol is in the open queue and alice is parked; neither can serve the
  // other.
  EXPECT_EQ(matchmaker_->parked("tictactoe"), 1);
  EXPECT_EQ(matchmaker_->queued("tictactoe"), 1);

  ShutdownServer();
  EXPECT_FALSE(future_a.get().got_game_over);
  EXPECT_FALSE(future_c.get().got_game_over);
}

TEST_F(BrokerIntegrationTest, RendezvousRejectsSelfAndEmptyPartner) {
  EXPECT_EQ(HelloStatus("alice", "player:alice").error_code(),
            grpc::StatusCode::INVALID_ARGUMENT);
  EXPECT_EQ(HelloStatus("alice", "player:").error_code(),
            grpc::StatusCode::INVALID_ARGUMENT);
  EXPECT_EQ(HelloStatus("alice", "sparring-partner").error_code(),
            grpc::StatusCode::INVALID_ARGUMENT);
  EXPECT_EQ(matchmaker_->parked("tictactoe"), 0);
}

// Two connections claiming the same seat of the same pairing are a scheduler
// bug, not a pairing: refusing the second keeps the first one's slot intact.
TEST_F(BrokerIntegrationTest, RendezvousRejectsDuplicateWaiter) {
  std::mt19937 gen_a(35);
  auto future_a = std::async(std::launch::async, [&] {
    return RunRandomClient("alice", "player:bob", &gen_a);
  });
  WaitForParked(1);

  EXPECT_EQ(HelloStatus("alice", "player:bob").error_code(),
            grpc::StatusCode::INVALID_ARGUMENT);
  EXPECT_EQ(matchmaker_->parked("tictactoe"), 1);

  ShutdownServer();
  EXPECT_FALSE(future_a.get().got_game_over);
}

class BrokerRendezvousTimeoutTest : public BrokerIntegrationTest {
 protected:
  auto MakeConfig() -> MatchmakerConfig override {
    MatchmakerConfig config = BrokerIntegrationTest::MakeConfig();
    config.rendezvous_timeout = std::chrono::milliseconds(200);
    return config;
  }
};

// A partner that never builds, or crashes on startup, must not park the other
// side forever: the wait is bounded and the stream is closed.
TEST_F(BrokerRendezvousTimeoutTest, ParkedClientIsReleasedWhenPartnerNeverComes) {
  std::mt19937 gen(36);
  const ClientResult result = RunRandomClient("alice", "player:ghost", &gen);
  EXPECT_FALSE(result.got_game_over);
  EXPECT_EQ(matchmaker_->parked("tictactoe"), 0);
  // No game was played, so no rating was touched.
  EXPECT_EQ(elo_store_->Get("tictactoe", "alice").wins() +
                elo_store_->Get("tictactoe", "alice").draws() +
                elo_store_->Get("tictactoe", "alice").losses(),
            0);
}

// --- per-game time budget --------------------------------------------------

class BrokerTimeBudgetTest : public BrokerIntegrationTest {
 protected:
  auto MakeConfig() -> MatchmakerConfig override {
    MatchmakerConfig config = BrokerIntegrationTest::MakeConfig();
    // Well under turn_timeout (300ms), so only the accumulated total can trip
    // it -- exactly the case the per-turn timeout cannot bound.
    config.game_time_budget = std::chrono::milliseconds(150);
    return config;
  }
};

TEST_F(BrokerTimeBudgetTest, ExhaustingTheBudgetLosesTheGame) {
  std::mt19937 gen(37);
  const ClientResult result =
      RunClient("plodder", "builtin:random", /*stall=*/false, &gen,
                /*think_time=*/std::chrono::milliseconds(100));
  ASSERT_TRUE(result.got_game_over);
  EXPECT_EQ(result.result, proto::GameOver::LOSS);
  // Each turn is comfortably inside turn_timeout; the total is not.
  EXPECT_EQ(result.reason, "time_budget");
  EXPECT_LT(result.new_elo, 1500.0);
}

TEST_F(BrokerTimeBudgetTest, FastClientIsUnaffectedByTheBudget) {
  std::mt19937 gen(38);
  const ClientResult result = RunRandomClient("sprinter", "builtin:random",
                                              &gen);
  ASSERT_TRUE(result.got_game_over);
  EXPECT_EQ(result.reason, "normal");
}

TEST_F(BrokerIntegrationTest, UnknownGameRejected) {
  auto channel = grpc::CreateChannel("127.0.0.1:" + std::to_string(port_),
                                     grpc::InsecureChannelCredentials());
  auto stub = proto::TournamentBroker::NewStub(channel);
  grpc::ClientContext context;
  auto stream = stub->Play(&context);
  proto::ClientMessage hello_msg;
  hello_msg.mutable_hello()->set_player_name("dave");
  hello_msg.mutable_hello()->set_game("chess");
  ASSERT_TRUE(stream->Write(hello_msg));
  stream->WritesDone();
  const grpc::Status status = stream->Finish();
  EXPECT_EQ(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
}

TEST_F(BrokerIntegrationTest, LeaderboardServesRatingsAndGames) {
  std::mt19937 gen(5);
  ASSERT_TRUE(RunRandomClient("erin", "builtin:minimax", &gen).rpc_ok);

  HttpLeaderboard http(/*port=*/0, elo_store_.get(), history_.get());
  ASSERT_TRUE(http.Start());
  const int http_port = http.bound_port();
  ASSERT_GT(http_port, 0);

  const auto http_get = [http_port](const std::string &path) -> std::string {
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    EXPECT_GE(fd, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(http_port));
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    EXPECT_EQ(connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)),
              0);
    const std::string request = "GET " + path + " HTTP/1.0\r\n\r\n";
    EXPECT_EQ(send(fd, request.data(), request.size(), 0),
              static_cast<ssize_t>(request.size()));
    std::string response;
    char buffer[4096];
    ssize_t n;
    while ((n = recv(fd, buffer, sizeof(buffer), 0)) > 0) {
      response.append(buffer, static_cast<size_t>(n));
    }
    close(fd);
    return response;
  };

  const std::string leaderboard = http_get("/api/leaderboard");
  EXPECT_NE(leaderboard.find("200 OK"), std::string::npos);
  EXPECT_NE(leaderboard.find("\"player\":\"erin\""), std::string::npos);
  EXPECT_NE(leaderboard.find("\"player\":\"builtin:minimax\""),
            std::string::npos);

  const std::string games = http_get("/api/games");
  EXPECT_NE(games.find("200 OK"), std::string::npos);
  EXPECT_NE(games.find("\"player0\":\"erin\""), std::string::npos);

  const std::string html = http_get("/");
  EXPECT_NE(html.find("Leaderboard"), std::string::npos);

  http.Stop();
}

// A peer that opens a stream and then says nothing holds a reactor open for as
// long as it likes. Keepalive does not cover it -- the connection is alive,
// just silent -- so the server bounds it with its own deadline.
class HelloDeadlineTest : public BrokerIntegrationTest {
 protected:
  auto HelloTimeout() -> std::chrono::milliseconds override {
    return std::chrono::milliseconds(200);
  }
};

TEST_F(HelloDeadlineTest, SilentConnectionIsClosed) {
  auto channel = grpc::CreateChannel("127.0.0.1:" + std::to_string(port_),
                                     grpc::InsecureChannelCredentials());
  auto stub = proto::TournamentBroker::NewStub(channel);
  grpc::ClientContext context;
  auto stream = stub->Play(&context);

  // Never send the hello; just wait for the server to give up.
  proto::ServerMessage msg;
  EXPECT_FALSE(stream->Read(&msg));
  const grpc::Status status = stream->Finish();
  EXPECT_EQ(status.error_code(), grpc::StatusCode::DEADLINE_EXCEEDED)
      << status.error_message();
}

TEST_F(HelloDeadlineTest, PromptClientIsUnaffected) {
  std::mt19937 gen(31);
  const ClientResult result =
      RunRandomClient("prompt", "builtin:random", &gen);
  EXPECT_TRUE(result.got_game_over);
  EXPECT_EQ(result.reason, "normal");
}

}  // namespace
}  // namespace tournament_broker
