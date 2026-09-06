// A sandbox fleet worker: builds submitted candidates and plays their games.
//
//   bazel run //game_mcts/tournament_server/sandbox/worker:sandbox_worker --
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
#include "absl/strings/str_split.h"
#include "game_mcts/tournament_server/proto/arena.grpc.pb.h"
#include "game_mcts/tournament_server/sandbox/worker/docker_backend.h"
#include "game_mcts/tournament_server/sandbox/worker/local_backend.h"

ABSL_FLAG(std::string, server, "localhost:50051",
          "host:port of the arena's SandboxFleet service");
ABSL_FLAG(std::string, worker_id, "",
          "Stable id for this worker; defaults to <hostname>-<pid>");
ABSL_FLAG(int, slots, 2, "Orders to run concurrently");
ABSL_FLAG(std::string, repo, "",
          "Repository to clone candidates into: a local path or a git URL "
          "(required)");
ABSL_FLAG(std::string, work_dir, "/tmp/arena_sandbox",
          "Where per-slot checkouts and bazel output bases live");
ABSL_FLAG(std::string, disk_cache, "",
          "Shared bazel --disk_cache across slots; defaults to "
          "<work_dir>/disk_cache");
ABSL_FLAG(std::string, backend, "local",
          "How orders are executed: local or docker");
ABSL_FLAG(std::string, docker_image, "",
          "Image for --backend=docker; must contain bazel matching the repo's "
          "MODULE.bazel.lock (required with --backend=docker)");
ABSL_FLAG(std::string, bazel, "bazel", "bazel binary (local backend)");
ABSL_FLAG(std::string, git, "git", "git binary");
ABSL_FLAG(std::string, bazel_flags, "",
          "Comma-separated extra bazel flags, e.g. --config=native");
ABSL_FLAG(bool, host_overlay, true,
          "Assemble each slot's overlay on the host and bind-mount the merged "
          "tree in, so containers need no CAP_SYS_ADMIN. Requires this worker "
          "to be able to mount overlayfs (root, or a user namespace). Turning "
          "it off falls back to mounting inside the container, which needs "
          "CAP_SYS_ADMIN and is not a security boundary");
ABSL_FLAG(bool, allow_build_network, false,
          "Let the build container reach the network. Off by default: a build "
          "that can fetch can also exfiltrate, and a submitted genrule is "
          "arbitrary code. The image must carry a warm bazel repository cache");
ABSL_FLAG(std::string, run_as_user, "",
          "Run containers as this user, e.g. \"1000:1000\". Empty leaves the "
          "image's default, which for most images is root");
ABSL_FLAG(double, container_cpus, 0.0, "CPU cap per container. 0 is unlimited");
ABSL_FLAG(int, pids_limit, 512, "Process cap per container. 0 is unlimited");
ABSL_FLAG(std::string, machine_class, "",
          "What kind of host this is, e.g. \"bench-c7i\". A graded problem can "
          "require one: a timing from a laptop and one from a server are not "
          "the same measurement, and a board that mixes them ranks the fleet "
          "rather than the submissions");
ABSL_FLAG(int, reconnect_delay_s, 5,
          "Delay before re-attaching after the arena drops the stream");

namespace {

using tournament_arena::DockerBackend;
using tournament_arena::DockerBackendConfig;
using tournament_arena::LocalBackend;
using tournament_arena::LocalBackendConfig;
using tournament_arena::OrderOutcome;
using tournament_arena::SandboxBackend;
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
  WorkerSession(SandboxBackend *backend, Stream *stream, int slots,
                std::string worker_id, std::string machine_class)
      : backend_(backend),
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
  // running -- the backend kills the process group or the containers by name.
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
    backend_->Cancel(order_id);
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
      SendProgress(order.order_id(), proto::OrderProgress::BUILDING);

      const OrderOutcome outcome = backend_->RunOrder(slot, order);

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

  SandboxBackend *backend_;  // not owned
  Stream *stream_;           // not owned
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

  if (absl::GetFlag(FLAGS_repo).empty()) {
    LOG(ERROR) << "Missing required --repo=<path or git url>";
    return 2;
  }

  const int slots = std::max(1, absl::GetFlag(FLAGS_slots));
  const std::string machine_class = absl::GetFlag(FLAGS_machine_class);
  const std::string worker_id = absl::GetFlag(FLAGS_worker_id).empty()
                                    ? DefaultWorkerId()
                                    : absl::GetFlag(FLAGS_worker_id);

  std::unique_ptr<SandboxBackend> backend;
  if (absl::GetFlag(FLAGS_backend) == "local") {
    LocalBackendConfig backend_config;
    backend_config.repo_url = absl::GetFlag(FLAGS_repo);
    backend_config.work_dir = absl::GetFlag(FLAGS_work_dir);
    backend_config.disk_cache =
        absl::GetFlag(FLAGS_disk_cache).empty()
            ? backend_config.work_dir / "disk_cache"
            : std::filesystem::path(absl::GetFlag(FLAGS_disk_cache));
    backend_config.bazel = absl::GetFlag(FLAGS_bazel);
    backend_config.git = absl::GetFlag(FLAGS_git);
    if (!absl::GetFlag(FLAGS_bazel_flags).empty()) {
      backend_config.bazel_flags = absl::StrSplit(
          absl::GetFlag(FLAGS_bazel_flags), ',', absl::SkipEmpty());
    }
    backend = std::make_unique<LocalBackend>(std::move(backend_config));
  } else if (absl::GetFlag(FLAGS_backend) == "docker") {
    if (absl::GetFlag(FLAGS_docker_image).empty()) {
      LOG(ERROR) << "--backend=docker requires --docker_image=<image>";
      return 2;
    }
    const std::filesystem::path repo(absl::GetFlag(FLAGS_repo));
    if (!std::filesystem::is_directory(repo)) {
      LOG(ERROR) << "--backend=docker needs --repo as a local path (it is "
                    "mounted into the containers), got '"
                 << absl::GetFlag(FLAGS_repo) << "'";
      return 2;
    }
    DockerBackendConfig backend_config;
    backend_config.repo_dir = repo;
    backend_config.work_dir = absl::GetFlag(FLAGS_work_dir);
    backend_config.disk_cache =
        absl::GetFlag(FLAGS_disk_cache).empty()
            ? backend_config.work_dir / "disk_cache"
            : std::filesystem::path(absl::GetFlag(FLAGS_disk_cache));
    backend_config.docker_image = absl::GetFlag(FLAGS_docker_image);
    backend_config.git = absl::GetFlag(FLAGS_git);
    backend_config.host_overlay = absl::GetFlag(FLAGS_host_overlay);
    backend_config.allow_build_network =
        absl::GetFlag(FLAGS_allow_build_network);
    backend_config.run_as_user = absl::GetFlag(FLAGS_run_as_user);
    backend_config.cpus = absl::GetFlag(FLAGS_container_cpus);
    backend_config.pids_limit = absl::GetFlag(FLAGS_pids_limit);
    if (!backend_config.host_overlay) {
      LOG(WARNING) << "--host_overlay=false: containers run with CAP_SYS_ADMIN "
                      "so they can mount their own overlay. Submitted build "
                      "code then runs privileged, which is not a boundary";
    }
    if (!absl::GetFlag(FLAGS_bazel_flags).empty()) {
      backend_config.bazel_flags = absl::StrSplit(
          absl::GetFlag(FLAGS_bazel_flags), ',', absl::SkipEmpty());
    }
    backend = std::make_unique<DockerBackend>(std::move(backend_config));
  } else {
    LOG(ERROR) << "Unsupported --backend='" << absl::GetFlag(FLAGS_backend)
               << "' (local or docker)";
    return 2;
  }

  LOG(INFO) << "Worker '" << worker_id << "' warming up " << slots
            << " slot(s) from " << absl::GetFlag(FLAGS_repo) << " via the "
            << backend->name() << " backend";
  std::string error;
  if (!backend->Warmup(slots, &error)) {
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
    hello.mutable_hello()->set_backend(backend->name());
    if (!stream->Write(hello)) {
      LOG(WARNING) << "Cannot reach the arena at "
                   << absl::GetFlag(FLAGS_server) << "; retrying";
    } else {
      LOG(INFO) << "Attached to " << absl::GetFlag(FLAGS_server) << " as '"
                << worker_id << "' with " << slots << " slot(s)";
      WorkerSession session(backend.get(), stream.get(), slots, worker_id,
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
                 << "); reconnecting in "
                 << absl::GetFlag(FLAGS_reconnect_delay_s) << "s";
    std::this_thread::sleep_for(
        std::chrono::seconds(absl::GetFlag(FLAGS_reconnect_delay_s)));
  }
}
