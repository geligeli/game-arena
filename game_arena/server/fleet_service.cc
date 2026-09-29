#include "game_arena/server/fleet_service.h"

#include <utility>

#include "absl/log/log.h"

namespace tournament_arena {

StreamFleetWorker::StreamFleetWorker(std::string worker_id, int slots,
                                     Stream *stream)
    : worker_id_(std::move(worker_id)),
      slots_(slots < 1 ? 1 : slots),
      stream_(stream) {}

StreamFleetWorker::~StreamFleetWorker() { Stop(); }

void StreamFleetWorker::Start() {
  writer_ = std::thread(&StreamFleetWorker::WriterLoop, this);
}

void StreamFleetWorker::Stop() {
  {
    std::lock_guard lock(mutex_);
    if (stopping_) {
      return;
    }
    stopping_ = true;
  }
  cv_.notify_all();
  if (writer_.joinable()) {
    writer_.join();
  }
}

bool StreamFleetWorker::Send(const proto::FleetMessage &msg) {
  {
    std::lock_guard lock(mutex_);
    if (stopping_) {
      return false;
    }
    if (outbox_.size() >= kMaxOutbox) {
      LOG(WARNING) << "Worker '" << worker_id_
                   << "': outbox full, treating as dead";
      return false;
    }
    outbox_.push_back(msg);
  }
  cv_.notify_one();
  return true;
}

void StreamFleetWorker::WriterLoop() {
  for (;;) {
    proto::FleetMessage msg;
    {
      std::unique_lock lock(mutex_);
      cv_.wait(lock, [&] { return stopping_ || !outbox_.empty(); });
      if (stopping_) {
        return;
      }
      msg = std::move(outbox_.front());
      outbox_.pop_front();
    }
    if (!stream_->Write(msg)) {
      std::lock_guard lock(mutex_);
      stopping_ = true;
      return;
    }
  }
}

FleetService::FleetService(Scheduler *scheduler, ArtifactStore *artifacts)
    : scheduler_(scheduler), artifacts_(artifacts) {}

grpc::Status FleetService::Attach(
    grpc::ServerContext * /*context*/,
    grpc::ServerReaderWriter<proto::FleetMessage, proto::WorkerMessage>
        *stream) {
  proto::WorkerMessage first;
  if (!stream->Read(&first) || !first.has_hello()) {
    return {grpc::StatusCode::INVALID_ARGUMENT,
            "first message must be a worker hello"};
  }
  const proto::WorkerHello &hello = first.hello();
  if (hello.worker_id().empty()) {
    return {grpc::StatusCode::INVALID_ARGUMENT, "worker_id is required"};
  }

  auto worker = std::make_shared<StreamFleetWorker>(hello.worker_id(),
                                                    hello.slots(), stream);
  worker->Start();
  scheduler_->AddWorker(worker);

  proto::WorkerMessage msg;
  while (stream->Read(&msg)) {
    if (msg.has_result()) {
      scheduler_->OnResult(hello.worker_id(), msg.result());
    } else if (msg.has_game()) {
      scheduler_->OnGame(msg.game());
    } else if (msg.has_progress()) {
      LOG(INFO) << "Worker '" << hello.worker_id() << "' order "
                << msg.progress().order_id() << ": "
                << proto::OrderProgress::Phase_Name(msg.progress().phase());
      scheduler_->OnProgress(msg.progress());
    }
  }

  // Detach first, so the scheduler stops handing it orders it cannot deliver.
  scheduler_->RemoveWorker(hello.worker_id());
  worker->Stop();
  return grpc::Status::OK;
}

grpc::Status FleetService::PutArtifact(
    grpc::ServerContext * /*context*/,
    grpc::ServerReader<proto::ArtifactChunk> *reader,
    proto::PutArtifactResponse *response) {
  if (artifacts_ == nullptr) {
    return {grpc::StatusCode::UNIMPLEMENTED, "this arena keeps no archive"};
  }
  std::string digest;
  std::string bytes;
  proto::ArtifactChunk chunk;
  while (reader->Read(&chunk)) {
    if (!digest.empty() && chunk.digest() != digest) {
      return {grpc::StatusCode::INVALID_ARGUMENT, "one upload is one artifact"};
    }
    digest = chunk.digest();
    if (bytes.size() + chunk.data().size() > kMaxArtifactBytes) {
      return {grpc::StatusCode::RESOURCE_EXHAUSTED, "artifact too large"};
    }
    bytes += chunk.data();
  }
  std::string error;
  if (!artifacts_->Put(digest, bytes, &error)) {
    return {grpc::StatusCode::INVALID_ARGUMENT, error};
  }
  response->set_digest(digest);
  return grpc::Status::OK;
}

grpc::Status FleetService::GetArtifact(
    grpc::ServerContext * /*context*/, const proto::GetArtifactRequest *request,
    grpc::ServerWriter<proto::ArtifactChunk> *writer) {
  const auto bytes =
      artifacts_ != nullptr ? artifacts_->Get(request->digest()) : std::nullopt;
  if (!bytes.has_value()) {
    return {grpc::StatusCode::NOT_FOUND, "no artifact " + request->digest()};
  }
  proto::ArtifactChunk chunk;
  chunk.set_digest(request->digest());
  for (std::size_t at = 0; at < bytes->size() || at == 0;
       at += kArtifactChunkBytes) {
    chunk.set_data(bytes->substr(at, kArtifactChunkBytes));
    if (!writer->Write(chunk)) {
      break;
    }
  }
  return grpc::Status::OK;
}

}  // namespace tournament_arena
