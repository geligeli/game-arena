#ifndef GAME_ARENA_GAME_ARENA_SERVER_FLEET_SERVICE_H
#define GAME_ARENA_GAME_ARENA_SERVER_FLEET_SERVICE_H

// Attach streams. Send() runs under the scheduler's lock, hence the outbox.

#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "game_arena/proto/arena.grpc.pb.h"
#include "game_arena/server/artifact_store.h"
#include "game_arena/server/fleet_worker.h"
#include "game_arena/server/scheduler.h"

namespace tournament_arena {

// Well under any gRPC message limit; a bot is ~15 MB.
inline constexpr std::size_t kArtifactChunkBytes = 1 << 20;
inline constexpr std::size_t kMaxArtifactBytes = 512 << 20;

class StreamFleetWorker : public FleetWorker {
 public:
  using Stream =
      grpc::ServerReaderWriter<proto::FleetMessage, proto::WorkerMessage>;

  // A worker that has stopped reading is broken, not busy.
  static constexpr std::size_t kMaxOutbox = 64;

  StreamFleetWorker(std::string worker_id, int slots, bool builds_artifacts,
                    Stream *stream);
  ~StreamFleetWorker() override;

  std::string worker_id() const override { return worker_id_; }
  int slots() const override { return slots_; }
  bool builds_artifacts() const override { return builds_artifacts_; }
  bool Send(const proto::FleetMessage &msg) override;

  void Start();
  void Stop();  // idempotent

 private:
  void WriterLoop();

  const std::string worker_id_;
  const int slots_;
  const bool builds_artifacts_;
  Stream *stream_;  // owned by the RPC handler, which outlives this object

  std::mutex mutex_;
  std::condition_variable cv_;
  std::deque<proto::FleetMessage> outbox_;
  bool stopping_ = false;
  std::thread writer_;
};

class FleetService final : public proto::SandboxFleet::Service {
 public:
  // Without |artifacts|, the archive RPCs are unimplemented.
  explicit FleetService(Scheduler *scheduler,
                        ArtifactStore *artifacts = nullptr);

  grpc::Status Attach(
      grpc::ServerContext *context,
      grpc::ServerReaderWriter<proto::FleetMessage, proto::WorkerMessage>
          *stream) override;
  grpc::Status PutArtifact(grpc::ServerContext *context,
                           grpc::ServerReader<proto::ArtifactChunk> *reader,
                           proto::PutArtifactResponse *response) override;
  grpc::Status GetArtifact(
      grpc::ServerContext *context, const proto::GetArtifactRequest *request,
      grpc::ServerWriter<proto::ArtifactChunk> *writer) override;

 private:
  Scheduler *scheduler_;      // not owned
  ArtifactStore *artifacts_;  // not owned; may be null
};

}  // namespace tournament_arena

#endif  // GAME_ARENA_GAME_ARENA_SERVER_FLEET_SERVICE_H
