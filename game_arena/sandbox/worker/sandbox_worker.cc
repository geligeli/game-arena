// A fleet worker: dials the arena and runs its orders on slot threads.

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
#include "game_arena/sandbox/worker/order_runner.h"

ABSL_FLAG(std::string, server, "localhost:50051",
          "host:port of the arena's SandboxFleet service. The only flag: what "
          "to build, what the sandbox may do, and where the tree comes from "
          "all arrive on each order, because two submissions are only "
          "comparable if they were built the same way");

namespace {

// Host facts come from ARENA_* variables (ARENA.md), keeping one flag. Also
// ARENA_WORKER_ID, stable across reconnects; default <hostname>-<pid>.
constexpr std::chrono::seconds kReconnectDelay{5};

std::string Hostname(const std::string &fallback) {
  char name[256] = {};
  return ::gethostname(name, sizeof(name) - 1) == 0 ? name : fallback;
}

std::string EnvOr(const char *name, const std::string &fallback) {
  const char *value = std::getenv(name);
  return value != nullptr && *value != '\0' ? std::string(value) : fallback;
}

using tournament_arena::OrderJobConfig;
using tournament_arena::OrderOutcome;
using tournament_arena::OrderRunner;
namespace proto = tournament_arena::proto;

using Stream =
    grpc::ClientReaderWriter<proto::WorkerMessage, proto::FleetMessage>;

// One per attached session: a dropped stream tears it down.
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

  // Not atomic: an order finishing in between just reports its result.
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
      const OrderOutcome outcome =
          runner_->RunOrder(slot, order,
                            [this](const std::string &order_id,
                                   proto::OrderProgress::Phase phase) {
                              SendProgress(order_id, phase);
                            });

      // Before the result, so the coordinator has them when it concludes.
      for (const auto &record : outcome.games) {
        proto::WorkerMessage game;
        game.mutable_game()->set_order_id(order.order_id());
        record.SerializeToString(game.mutable_game()->mutable_record());
        Write(game);
      }

      proto::WorkerMessage message;
      auto *result = message.mutable_result();
      *result = outcome.result;
      result->set_order_id(order.order_id());
      result->set_worker_id(worker_id_);
      result->set_machine_class(machine_class_);

      LOG(INFO) << "slot " << slot << ": order " << order.order_id()
                << " done, build_ok=" << result->build_ok()
                << " games=" << result->games_played()
                << (result->error().empty() ? "" : " error=" + result->error());
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

  // gRPC's sync streams allow one writer at a time.
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

int main(int argc, char **argv) {
  absl::ParseCommandLine(argc, argv);
  absl::InitializeLog();
  absl::SetStderrThreshold(absl::LogSeverityAtLeast::kInfo);

  const int slots = std::max(1, std::atoi(EnvOr("ARENA_SLOTS", "2").c_str()));
  const std::string machine_class = EnvOr("ARENA_MACHINE_CLASS", "");
  const std::string host = Hostname("");
  const std::string worker_id =
      EnvOr("ARENA_WORKER_ID", (host.empty() ? "worker" : host) + "-" +
                                   std::to_string(::getpid()));

  const std::filesystem::path work_dir =
      EnvOr("ARENA_WORK_DIR", "/tmp/arena_sandbox");
  OrderJobConfig job_config;
  job_config.work_dir = work_dir;
  job_config.volume_prefix =
      EnvOr("ARENA_VOLUME_PREFIX", "arena-" + Hostname("host"));
  job_config.bind_output_base_dir = EnvOr("ARENA_BIND_OUTPUT_BASE", "");
  job_config.bind_disk_cache_dir = EnvOr("ARENA_BIND_DISK_CACHE", "");

  // No unsandboxed backend, and no flag for one: submitted code is arbitrary.
  if (process::ResolveExecutable("docker").empty()) {
    LOG(ERROR) << "no docker on PATH: a worker builds and runs every order in "
                  "a container, so this one would refuse all of them";
    return 1;
  }
  sandbox_exec::ContainerEngine container_engine({});
  OrderRunner runner(/*process_engine=*/nullptr, &container_engine,
                     std::move(job_config), machine_class);

  LOG(INFO) << "Worker '" << worker_id << "' warming up " << slots
            << " slot(s) under " << work_dir << ", container engine(s)"
            << (machine_class.empty()
                    ? ", no machine class (ARENA_MACHINE_CLASS unset: graded "
                      "problems that require one will refuse this worker)"
                    : ", machine class '" + machine_class + "'");
  std::string error;
  if (!runner.Warmup(slots, &error)) {
    LOG(ERROR) << "Cannot prepare slots: " << error;
    return 1;
  }

  // Forever: an arena restart must not retire a fleet host.
  for (;;) {
    auto channel = grpc::CreateChannel(absl::GetFlag(FLAGS_server),
                                       grpc::InsecureChannelCredentials());
    auto stub = proto::SandboxFleet::NewStub(channel);
    grpc::ClientContext context;
    std::unique_ptr<Stream> stream = stub->Attach(&context);

    proto::WorkerMessage hello;
    hello.mutable_hello()->set_worker_id(worker_id);
    hello.mutable_hello()->set_slots(slots);
    // Up front too, so the arena can schedule on it.
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
