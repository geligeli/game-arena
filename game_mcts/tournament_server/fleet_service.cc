#include "cpp/tournament_server/fleet_service.h"

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

auto StreamFleetWorker::Send(const proto::FleetMessage &msg) -> bool {
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
      // The reader loop will see the stream break too and detach the worker;
      // stopping here just avoids piling up writes that cannot land.
      std::lock_guard lock(mutex_);
      stopping_ = true;
      return;
    }
  }
}

FleetService::FleetService(Scheduler *scheduler) : scheduler_(scheduler) {}

auto FleetService::Attach(
    grpc::ServerContext * /*context*/,
    grpc::ServerReaderWriter<proto::FleetMessage, proto::WorkerMessage>
        *stream) -> grpc::Status {
  proto::WorkerMessage first;
  if (!stream->Read(&first) || !first.has_hello()) {
    return {grpc::StatusCode::INVALID_ARGUMENT,
            "first message must be a worker hello"};
  }
  const proto::WorkerHello &hello = first.hello();
  if (hello.worker_id().empty()) {
    return {grpc::StatusCode::INVALID_ARGUMENT, "worker_id is required"};
  }

  auto worker = std::make_shared<StreamFleetWorker>(
      hello.worker_id(), hello.slots(), stream);
  worker->Start();
  scheduler_->AddWorker(worker);

  proto::WorkerMessage msg;
  while (stream->Read(&msg)) {
    if (msg.has_result()) {
      scheduler_->OnResult(hello.worker_id(), msg.result());
    } else if (msg.has_progress()) {
      LOG(INFO) << "Worker '" << hello.worker_id() << "' order "
                << msg.progress().order_id() << ": "
                << proto::OrderProgress::Phase_Name(msg.progress().phase());
    }
    // Heartbeats need no action: the stream itself is the liveness signal.
  }

  // Detach before stopping the writer, so the scheduler stops handing this
  // worker orders it can no longer deliver.
  scheduler_->RemoveWorker(hello.worker_id());
  worker->Stop();
  return grpc::Status::OK;
}

}  // namespace tournament_arena
