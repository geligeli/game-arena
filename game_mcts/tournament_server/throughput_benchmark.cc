// In-process load test for the broker: move throughput vs concurrent games.
//
// Server and clients share one process and talk real gRPC over loopback. That
// is deliberate -- the gRPC serving layer is the thing being measured, so
// bypassing it would measure nothing. Everything else is pushed out of the way:
//
//   * clients are callback-API reactors, not threads. A thread-per-client load
//     generator saturates the machine long before the server does, which caps
//     the interesting range at a few hundred clients instead of thousands.
//   * the game is `bench`, where every action is legal, so each client replays
//     one pre-serialized action forever and neither side spends time on rules.
//     Games effectively never end, so a run holds a *steady* number of
//     concurrent games rather than measuring stream churn.
//   * logging is compiled out (SetMinLogLevel), not merely muted.
//   * --data_dir should be a tmpfs.
//
// It compiles unchanged against the pre-refactor broker (thread per client plus
// thread per game) and the current one (callback reactors plus event-driven
// games on a worker pool), so the two are directly comparable. Only fields both
// revisions have are set.
//
//   bazel run -c opt //game_mcts/tournament_server:throughput_benchmark --
//       --label=new --games=1,10,100,1000,5000

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <grpcpp/grpcpp.h>
#include <grpcpp/support/client_callback.h>

#include "absl/flags/flag.h"
#include "absl/flags/parse.h"
#include "absl/log/globals.h"
#include "absl/log/initialize.h"
#include "cpp/benchgame/bench_game.h"
#include "cpp/benchgame/bench_serialization.h"
#include "cpp/tournament_server/broker_service.h"
#include "cpp/tournament_server/elo_store.h"
#include "cpp/tournament_server/game_history.h"
#include "cpp/tournament_server/matchmaker.h"
#include "cpp/tournament_server/tournament_broker.grpc.pb.h"

ABSL_FLAG(std::string, label, "new",
          "Implementation name recorded in the CSV (e.g. old/new)");
ABSL_FLAG(std::string, games, "1,10,50,100,250,500,1000,2500,5000",
          "Comma-separated sweep of concurrent games (2 clients each)");
ABSL_FLAG(int, warmup_ms, 2500, "Discarded startup window per data point");
ABSL_FLAG(int, measure_ms, 4000, "Measured window per data point");
ABSL_FLAG(int, turn_timeout_ms, 60000,
          "Per-turn limit. Generous: a backlogged server must show up as low "
          "throughput, not as timed-out games");
ABSL_FLAG(int, clients_per_channel, 64,
          "Streams per channel. One channel for everything would serialize "
          "thousands of streams onto a single HTTP/2 connection");
ABSL_FLAG(std::string, data_dir, "/dev/shm/broker_bench",
          "Scratch dir for ratings and history; use a tmpfs");
ABSL_FLAG(bool, header, true, "Print the CSV header line");

namespace tournament_broker {
namespace {

using Clock = std::chrono::steady_clock;

std::atomic<uint64_t> g_moves{0};

auto FixedAction() -> const std::string & {
  static const std::string *kAction = new std::string(
      mcts::GameSerializationTraits<benchgame::BenchGame>::ActionToProto(0)
          .SerializeAsString());
  return *kAction;
}

// One load-generating client, driven entirely by gRPC's callback API: it owns
// no thread, so thousands of them cost memory rather than scheduler pressure.
class BenchClient final
    : public grpc::ClientBidiReactor<proto::ClientMessage,
                                     proto::ServerMessage> {
 public:
  BenchClient(proto::TournamentBroker::Stub *stub, const std::string &name) {
    auto *hello = out_.mutable_hello();
    hello->set_player_name(name);
    hello->set_game("bench");
    hello->set_opponent("any");
    action_.mutable_action()->set_action(FixedAction());

    stub->async()->Play(&context_, this);
    write_in_flight_ = true;
    StartWrite(&out_);
    StartRead(&in_);
    StartCall();
  }

  void OnReadDone(bool ok) override {
    if (!ok) {
      return;  // Stream ending; OnDone follows.
    }
    if (in_.has_your_turn()) {
      g_moves.fetch_add(1, std::memory_order_relaxed);
      bool send_now = false;
      {
        std::lock_guard lock(mu_);
        // Reads and writes complete on independent threads, so the
        // one-write-in-flight rule needs a guard here just as it does server
        // side. At most one action is ever pending: the server does not ask
        // again until this one lands.
        if (write_in_flight_) {
          pending_action_ = true;
        } else {
          write_in_flight_ = true;
          send_now = true;
        }
      }
      if (send_now) {
        StartWrite(&action_);
      }
    }
    StartRead(&in_);
  }

  void OnWriteDone(bool ok) override {
    bool send_next = false;
    {
      std::lock_guard lock(mu_);
      write_in_flight_ = false;
      if (ok && pending_action_) {
        pending_action_ = false;
        write_in_flight_ = true;
        send_next = true;
      }
    }
    if (send_next) {
      StartWrite(&action_);
    }
  }

  void OnDone(const grpc::Status & /*status*/) override {
    {
      std::lock_guard lock(mu_);
      done_ = true;
    }
    cv_.notify_all();
  }

  void Cancel() { context_.TryCancel(); }

  void Await() {
    std::unique_lock lock(mu_);
    cv_.wait(lock, [&] { return done_; });
  }

 private:
  grpc::ClientContext context_;
  proto::ClientMessage out_;     // the hello
  proto::ClientMessage action_;  // the one action, replayed forever
  proto::ServerMessage in_;

  std::mutex mu_;
  std::condition_variable cv_;
  bool write_in_flight_ = false;
  bool pending_action_ = false;
  bool done_ = false;
};

struct Point {
  int games;
  uint64_t moves;
  double seconds;
  int threads;
};

auto ThreadCount() -> int {
  std::error_code ec;
  int count = 0;
  for (auto it = std::filesystem::directory_iterator("/proc/self/task", ec);
       !ec && it != std::filesystem::end(it); it.increment(ec)) {
    ++count;
  }
  return count;
}

auto MeasureOnce(int concurrent_games) -> Point {
  const int clients = concurrent_games * 2;
  const std::filesystem::path dir =
      std::filesystem::path(absl::GetFlag(FLAGS_data_dir)) /
      ("g" + std::to_string(concurrent_games));
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);

  EloStore elo_store(dir / "ratings.pb", /*k_factor=*/32.0);
  GameHistory history(dir / "games");

  MatchmakerConfig config;  // Only fields both revisions have.
  config.turn_timeout =
      std::chrono::milliseconds(absl::GetFlag(FLAGS_turn_timeout_ms));
  config.max_moves_per_game = 100000000;  // never the reason a game ends
  Matchmaker matchmaker(config, &elo_store, &history);
  BrokerService service(&matchmaker);

  int port = 0;
  grpc::ServerBuilder builder;
  builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(),
                           &port);
  builder.RegisterService(&service);
  std::unique_ptr<grpc::Server> server = builder.BuildAndStart();
  const std::string target = "127.0.0.1:" + std::to_string(port);

  const int per_channel = std::max(1, absl::GetFlag(FLAGS_clients_per_channel));
  const int num_channels = (clients + per_channel - 1) / per_channel;
  std::vector<std::shared_ptr<grpc::Channel>> channels;
  std::vector<std::unique_ptr<proto::TournamentBroker::Stub>> stubs;
  for (int i = 0; i < num_channels; ++i) {
    grpc::ChannelArguments args;
    // Distinct args stop gRPC folding these into one shared subchannel.
    args.SetInt("bench.channel_id", i);
    channels.push_back(grpc::CreateCustomChannel(
        target, grpc::InsecureChannelCredentials(), args));
    stubs.push_back(proto::TournamentBroker::NewStub(channels.back()));
  }

  g_moves.store(0);
  std::vector<std::unique_ptr<BenchClient>> bench_clients;
  bench_clients.reserve(static_cast<size_t>(clients));
  for (int i = 0; i < clients; ++i) {
    bench_clients.push_back(std::make_unique<BenchClient>(
        stubs[static_cast<size_t>(i / per_channel)].get(),
        "c" + std::to_string(i)));
  }

  std::this_thread::sleep_for(
      std::chrono::milliseconds(absl::GetFlag(FLAGS_warmup_ms)));
  const uint64_t moves0 = g_moves.load();
  const auto t0 = Clock::now();

  std::this_thread::sleep_for(
      std::chrono::milliseconds(absl::GetFlag(FLAGS_measure_ms)));
  const auto t1 = Clock::now();
  const uint64_t moves1 = g_moves.load();
  const int thread_count = ThreadCount();

  for (auto &client : bench_clients) {
    client->Cancel();
  }
  for (auto &client : bench_clients) {
    client->Await();
  }
  bench_clients.clear();
  server->Shutdown(std::chrono::system_clock::now() + std::chrono::seconds(10));
  matchmaker.Drain();
  server.reset();
  std::filesystem::remove_all(dir);

  return Point{.games = concurrent_games,
               .moves = moves1 - moves0,
               .seconds = std::chrono::duration<double>(t1 - t0).count(),
               .threads = thread_count};
}

}  // namespace
}  // namespace tournament_broker

auto main(int argc, char **argv) -> int {
  absl::ParseCommandLine(argc, argv);
  absl::InitializeLog();
  // Compiled out, not merely muted: the broker logs once per join and once per
  // finished game, which at these rates would be a large share of the work.
  absl::SetMinLogLevel(absl::LogSeverityAtLeast::kError);
  absl::SetStderrThreshold(absl::LogSeverityAtLeast::kError);

  std::vector<int> sweep;
  {
    const std::string spec = absl::GetFlag(FLAGS_games);
    size_t start = 0;
    while (start <= spec.size()) {
      const size_t comma = spec.find(',', start);
      const std::string token = spec.substr(
          start,
          comma == std::string::npos ? std::string::npos : comma - start);
      if (!token.empty()) {
        sweep.push_back(std::stoi(token));
      }
      if (comma == std::string::npos) {
        break;
      }
      start = comma + 1;
    }
  }

  const std::string label = absl::GetFlag(FLAGS_label);
  if (absl::GetFlag(FLAGS_header)) {
    std::cout << "impl,games,clients,moves,seconds,moves_per_sec,threads\n";
  }
  for (const int games : sweep) {
    const auto point = tournament_broker::MeasureOnce(games);
    std::cout << label << ',' << point.games << ',' << (point.games * 2) << ','
              << point.moves << ',' << point.seconds << ','
              << (static_cast<double>(point.moves) / point.seconds) << ','
              << point.threads << '\n'
              << std::flush;
  }
  return 0;
}
