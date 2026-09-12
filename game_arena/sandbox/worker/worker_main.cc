// A sandbox fleet worker: builds submitted candidates and plays their games.
//
//   bazel run //game_arena/sandbox/worker:sandbox_worker --
//       --server=localhost:50051 --repo=/large_nfs/game-mcts --slots=2
//
// The worker dials the arena, so a fleet can be attached from any host that
// has the repo, bazel and a route to the broker -- no inbound port, no
// registration, nothing to configure on the server. Adding capacity is
// starting another one of these.
//
// Orders are pulled off the Attach stream onto a fixed set of slot threads.
// Each slot owns a checkout and a bazel output base, so builds run in parallel
// without sharing a workspace lock.

#include <grpcpp/grpcpp.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "absl/flags/flag.h"
#include "absl/flags/parse.h"
#include "absl/log/globals.h"
#include "absl/log/initialize.h"
#include "absl/log/log.h"
#include "game_arena/common/process/process.h"
#include "game_arena/proto/arena.grpc.pb.h"
#include "game_arena/sandbox/exec/container_engine.h"
#include "game_arena/sandbox/exec/process_engine.h"
#include "game_arena/sandbox/worker/order_runner.h"

ABSL_FLAG(std::string, server, "localhost:50051",
          "host:port of the arena's SandboxFleet service. The only flag: what "
          "to build, what the sandbox may do, and where the tree comes from "
          "all arrive on each order, because two submissions are only "
          "comparable if they were built the same way");

namespace {

// The three things a coordinator cannot know, because they are facts about
// this host rather than about the problem. Environment rather than flags, so
// the flag surface stays at one and so a systemd unit or a container spec is
// the natural place to state them.
//
//   ARENA_MACHINE_CLASS   what kind of host this is, e.g. "bench-c7i". A
//                         graded problem can require one, and nothing can
//                         derive a semantic label -- without it that gate is
//                         unenforceable.
//   ARENA_SLOTS           how many orders to run at once: how much of this
//                         box to lend the arena.
//   ARENA_WORK_DIR        where the per-slot checkouts and bazel output bases
//                         live. Keep it off the repo: a work dir inside makes
//                         `bazel test //...` descend into the worker's own
//                         clone.
//   ARENA_WORKER_ID       stable across reconnects; defaults to
//                         <hostname>-<pid>.
// How long to wait before re-attaching. Not configurable: nothing about a
// problem or a host makes a different number right.
constexpr std::chrono::seconds kReconnectDelay{5};

auto EnvOr(const char *name, const std::string &fallback) -> std::string {
  const char *value = std::getenv(name);
  return value != nullptr && *value != '\0' ? std::string(value) : fallback;
}

using tournament_arena::OrderJobConfig;
using tournament_arena::OrderOutcome;
using tournament_arena::OrderRunner;
namespace proto = tournament_arena::proto;

using Stream =
    grpc::ClientReaderWriter<proto::WorkerMessage, proto::FleetMessage>;

auto DefaultWorkerId() -> std::string {
  char hostname[256] = {};
  if (::gethostname(hostname, sizeof(hostname) - 1) != 0) {
    hostname[0] = '\0';
  }
  const std::string host = hostname[0] != '\0' ? hostname : "worker";
  return host + "-" + std::to_string(::getpid());
}

// Runs orders on a fixed pool of slot threads and reports results back on the
// stream. One instance per attached session: when the stream drops, the
// session is torn down and a fresh one is built on reconnect.
class WorkerSession {
 public:
  WorkerSession(OrderRunner *runner, Stream *stream, int slots,
                std::string worker_id, std::string machine_class)
      : runner_(runner),
        stream_(stream),
        worker_id_(std::move(worker_id)),
        machine_class_(std::move(machine_class)) {
    for (int slot = 0; slot < slots; ++slot) {
      threads_.emplace_back([this, slot] { SlotLoop(slot); });
    }
  }

  ~WorkerSession() {
    Stop();
    for (std::thread &thread : threads_) {
      if (thread.joinable()) {
        thread.join();
      }
    }
  }

  void Enqueue(const proto::WorkOrder &order) {
    {
      std::lock_guard lock(mutex_);
      queue_.push_back(order);
    }
    cv_.notify_one();
  }

  // Drops the order if it is still queued, and stops it if it is already
  // running -- the engine kills the process group or the containers by name.
  //
  // The two halves are deliberately not one atomic step. An order that finishes
  // between them just reports its result, which the arena already tolerates:
  // it asked for the slot back, and it gets the slot back either way.
  void Cancel(const std::string &order_id) {
    {
      std::lock_guard lock(mutex_);
      std::erase_if(queue_, [&](const proto::WorkOrder &order) {
        return order.order_id() == order_id;
      });
    }
    runner_->Cancel(order_id);
  }

  void Stop() {
    {
      std::lock_guard lock(mutex_);
      stopping_ = true;
    }
    cv_.notify_all();
  }

 private:
  void SlotLoop(int slot) {
    for (;;) {
      proto::WorkOrder order;
      {
        std::unique_lock lock(mutex_);
        cv_.wait(lock, [&] { return stopping_ || !queue_.empty(); });
        if (stopping_) {
          return;
        }
        order = std::move(queue_.front());
        queue_.pop_front();
      }

      LOG(INFO) << "slot " << slot << ": order " << order.order_id()
                << " candidate " << order.candidate().candidate_id() << " vs "
                << order.opponent_spec() << " (" << order.num_games()
                << " games)";
      // Progress now comes from the engine, phase by phase, rather than one
      // BUILDING guess before anything started: CLONING and RUNNING were dead
      // enum values until the engine reported its phases.
      const OrderOutcome outcome =
          runner_->RunOrder(slot, order,
                            [this](const std::string &order_id,
                                   proto::OrderProgress::Phase phase) {
                              SendProgress(order_id, phase);
                            });

      proto::WorkerMessage message;
      auto *result = message.mutable_result();
      result->set_order_id(order.order_id());
      result->set_build_ok(outcome.build_ok);
      result->set_build_log(outcome.build_log);
      result->set_games_played(outcome.games_played);
      result->set_wins(outcome.wins);
      result->set_draws(outcome.draws);
      result->set_losses(outcome.losses);
      result->set_elo(outcome.elo);
      result->set_error(outcome.error);
      // Whose build broke, when one did. An order builds both sides of a
      // match, and the opponent failing to compile is not the submitter's
      // fault -- without this the arena retires the wrong submission.
      result->set_build_failed_candidate_id(outcome.build_failed_candidate_id);
      result->set_worker_id(worker_id_);
      result->set_machine_class(machine_class_);
      for (const auto &[name, value] : outcome.metrics) {
        (*result->mutable_metrics())[name] = value;
      }

      LOG(INFO) << "slot " << slot << ": order " << order.order_id()
                << " done, build_ok=" << outcome.build_ok
                << " games=" << outcome.games_played
                << (outcome.error.empty() ? "" : " error=" + outcome.error);
      Write(message);
    }
  }

  void SendProgress(const std::string &order_id,
                    proto::OrderProgress::Phase phase) {
    proto::WorkerMessage message;
    message.mutable_progress()->set_order_id(order_id);
    message.mutable_progress()->set_phase(phase);
    Write(message);
  }

  // gRPC's sync streams allow one writer at a time, and slot threads finish
  // whenever they finish.
  void Write(const proto::WorkerMessage &message) {
    std::lock_guard lock(write_mutex_);
    stream_->Write(message);
  }

  OrderRunner *runner_;  // not owned
  Stream *stream_;       // not owned
  const std::string worker_id_;
  const std::string machine_class_;

  std::mutex mutex_;
  std::condition_variable cv_;
  std::deque<proto::WorkOrder> queue_;
  bool stopping_ = false;

  std::mutex write_mutex_;
  std::vector<std::thread> threads_;
};

}  // namespace

auto main(int argc, char **argv) -> int {
  absl::ParseCommandLine(argc, argv);
  absl::InitializeLog();
  absl::SetStderrThreshold(absl::LogSeverityAtLeast::kInfo);

  const int slots = std::max(1, std::atoi(EnvOr("ARENA_SLOTS", "2").c_str()));
  const std::string machine_class = EnvOr("ARENA_MACHINE_CLASS", "");
  const std::string worker_id = EnvOr("ARENA_WORKER_ID", DefaultWorkerId());

  // This host's own layout, and nothing about any problem.
  const std::filesystem::path work_dir =
      EnvOr("ARENA_WORK_DIR", "/tmp/arena_sandbox");
  OrderJobConfig job_config;
  job_config.work_dir = work_dir;
  job_config.disk_cache = work_dir / "disk_cache";

  // Both engines, always. Which one an order runs on is the problem's
  // decision -- it names an image or it does not -- so a worker does not get
  // to have an opinion, and does not need a flag to express one.
  auto process_engine = std::make_unique<sandbox_exec::ProcessEngine>();
  std::unique_ptr<sandbox_exec::ContainerEngine> container_engine;
  if (process::ResolveExecutable("docker").empty()) {
    LOG(WARNING) << "no docker on PATH: this worker can only run orders that "
                    "name no image, and will refuse the rest rather than run "
                    "submitted code unsandboxed";
  } else {
    container_engine = std::make_unique<sandbox_exec::ContainerEngine>(
        sandbox_exec::ContainerEngineConfig{});
  }

  OrderRunner runner(process_engine.get(), container_engine.get(),
                     std::move(job_config), machine_class);

  // No repository named here: each order says where its tree comes from.
  LOG(INFO) << "Worker '" << worker_id << "' warming up " << slots
            << " slot(s) under " << work_dir << ", " << runner.engines()
            << " engine(s)"
            << (machine_class.empty()
                    ? ", no machine class (ARENA_MACHINE_CLASS unset: graded "
                      "problems that require one will refuse this worker)"
                    : ", machine class '" + machine_class + "'");
  std::string error;
  if (!runner.Warmup(slots, &error)) {
    LOG(ERROR) << "Cannot prepare slots: " << error;
    return 1;
  }

  // Reconnects forever: the arena restarting, or a network blip, must not take
  // a fleet host out of service permanently.
  for (;;) {
    auto channel = grpc::CreateChannel(absl::GetFlag(FLAGS_server),
                                       grpc::InsecureChannelCredentials());
    auto stub = proto::SandboxFleet::NewStub(channel);
    grpc::ClientContext context;
    std::unique_ptr<Stream> stream = stub->Attach(&context);

    proto::WorkerMessage hello;
    hello.mutable_hello()->set_worker_id(worker_id);
    hello.mutable_hello()->set_slots(slots);
    hello.mutable_hello()->set_backend(runner.engines());
    // The class of host this is. Stamped on every result too, but the
    // arena cannot schedule on what it is never told up front.
    hello.mutable_hello()->set_machine_class(machine_class);
    if (!stream->Write(hello)) {
      LOG(WARNING) << "Cannot reach the arena at "
                   << absl::GetFlag(FLAGS_server) << "; retrying";
    } else {
      LOG(INFO) << "Attached to " << absl::GetFlag(FLAGS_server) << " as '"
                << worker_id << "' with " << slots << " slot(s)";
      WorkerSession session(&runner, stream.get(), slots, worker_id,
                            machine_class);
      proto::FleetMessage message;
      while (stream->Read(&message)) {
        if (message.has_order()) {
          session.Enqueue(message.order());
        } else if (message.has_cancel()) {
          session.Cancel(message.cancel().order_id());
        }
      }
      // Stops the slot threads before the stream goes away under them.
      session.Stop();
    }

    stream->WritesDone();
    const grpc::Status status = stream->Finish();
    LOG(WARNING) << "Detached from the arena ("
                 << (status.ok() ? "stream closed" : status.error_message())
                 << "); reconnecting in " << kReconnectDelay.count() << "s";
    std::this_thread::sleep_for(kReconnectDelay);
  }
}
