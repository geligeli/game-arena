#ifndef RISK_GAME_AI_CPP_TOURNAMENT_SERVER_FLEET_SERVICE_H
#define RISK_GAME_AI_CPP_TOURNAMENT_SERVER_FLEET_SERVICE_H

// The sandbox fleet's gRPC surface: one long-lived Attach stream per worker.
//
// Synchronous, unlike the broker's Play stream, and deliberately so. Workers
// are counted in units of hosts, not players, so a thread each is nothing --
// whereas the broker had to stop paying a thread per queued bot. The
// simplicity is worth more here than the thread.
//
// Writes go through a per-worker outbox drained by its own thread. The
// scheduler calls Send() while holding its lock, so writing to the socket
// inline would let one unresponsive host stall dispatch for every other.

#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "game_mcts/tournament_server/arena.grpc.pb.h"
#include "game_mcts/tournament_server/fleet_worker.h"
#include "game_mcts/tournament_server/scheduler.h"

namespace tournament_arena {

// A FleetWorker backed by one Attach stream.
class StreamFleetWorker
    : public FleetWorker,
      public std::enable_shared_from_this<StreamFleetWorker> {
 public:
  using Stream =
      grpc::ServerReaderWriter<proto::FleetMessage, proto::WorkerMessage>;

  // A worker that has stopped reading is broken, not busy: the queue is a
  // liveness signal rather than a buffer to grow.
  static constexpr std::size_t kMaxOutbox = 64;

  StreamFleetWorker(std::string worker_id, int slots, Stream *stream);
  ~StreamFleetWorker() override;

  auto worker_id() const -> std::string override { return worker_id_; }
  auto slots() const -> int override { return slots_; }
  auto Send(const proto::FleetMessage &msg) -> bool override;

  // Starts the writer thread. Call once, before the stream is used.
  void Start();
  // Stops the writer thread and joins it. Idempotent.
  void Stop();

 private:
  void WriterLoop();

  const std::string worker_id_;
  const int slots_;
  Stream *stream_;  // owned by the RPC handler, which outlives this object

  std::mutex mutex_;
  std::condition_variable cv_;
  std::deque<proto::FleetMessage> outbox_;
  bool stopping_ = false;
  std::thread writer_;
};

class FleetService final : public proto::SandboxFleet::Service {
 public:
  explicit FleetService(Scheduler *scheduler);

  auto Attach(
      grpc::ServerContext *context,
      grpc::ServerReaderWriter<proto::FleetMessage, proto::WorkerMessage>
          *stream) -> grpc::Status override;

 private:
  Scheduler *scheduler_;  // not owned
};

}  // namespace tournament_arena

#endif  // RISK_GAME_AI_CPP_TOURNAMENT_SERVER_FLEET_SERVICE_H
