#include "game_arena/sandbox/worker/order_runner.h"

#include <filesystem>
#include <string>
#include <system_error>
#include <utility>

#include "game_arena/sandbox/exec/sandbox_job.pb.h"

namespace tournament_arena {

namespace {

// Phases by name: the engine does not know that "build" is a build.
class ProgressObserver final : public sandbox_exec::Observer {
 public:
  ProgressObserver(std::string order_id, const OrderRunner::ProgressSink *sink)
      : order_id_(std::move(order_id)), sink_(sink) {}

  void OnWorkspaceReady(const std::string &) override {
    Report(proto::OrderProgress::PREPARING);
  }
  void OnPhaseStarted(const std::string &, const std::string &phase) override {
    Report(phase == "build" ? proto::OrderProgress::BUILDING
                            : proto::OrderProgress::RUNNING);
  }

 private:
  void Report(proto::OrderProgress::Phase phase) {
    if (sink_ != nullptr && *sink_) {
      (*sink_)(order_id_, phase);
    }
  }

  const std::string order_id_;
  const OrderRunner::ProgressSink *const sink_;
};

}  // namespace

OrderRunner::OrderRunner(sandbox_exec::Engine *process_engine,
                         sandbox_exec::Engine *container_engine,
                         OrderJobConfig config, std::string machine_class)
    : process_engine_(process_engine),
      container_engine_(container_engine),
      config_(std::move(config)),
      machine_class_(std::move(machine_class)) {}

bool OrderRunner::Warmup(int slots, std::string *error) {
  // Bind-mount sources too: docker would create a missing one owned by root.
  std::error_code ec;
  std::filesystem::create_directories(config_.disk_cache, ec);
  if (!config_.bind_disk_cache_dir.empty()) {
    std::filesystem::create_directories(config_.bind_disk_cache_dir, ec);
  }
  for (int slot = 0; slot < slots; ++slot) {
    const std::filesystem::path slot_dir =
        config_.work_dir / ("slot" + std::to_string(slot));
    std::filesystem::create_directories(slot_dir / "bazel_output_base", ec);
    std::filesystem::create_directories(slot_dir / "scratch", ec);
    std::filesystem::create_directories(SlotLogDir(config_, slot), ec);
    if (!config_.bind_output_base_dir.empty()) {
      std::filesystem::create_directories(
          config_.bind_output_base_dir / ("slot" + std::to_string(slot)), ec);
    }
    if (ec) {
      *error = "cannot create slot directories under " +
               config_.work_dir.string() + ": " + ec.message();
      return false;
    }
  }
  return true;
}

sandbox_exec::Engine *OrderRunner::EngineFor(
    const proto::WorkOrder &order) const {
  // A tournament always names an image; the process engine is test-only.
  return order.sandbox().image().empty() ? process_engine_ : container_engine_;
}

std::string OrderRunner::Refusal(const proto::WorkOrder &order) const {
  const sandbox_exec::Engine *engine = EngineFor(order);
  if (engine == nullptr) {
    return order.sandbox().image().empty()
               ? "this worker cannot run unsandboxed orders"
               : "this problem needs a container and this worker has no "
                 "container engine";
  }
  const std::string &required = order.grade().require_machine_class();
  if (!required.empty() && required != machine_class_) {
    // A number from the wrong kind of host would rank the fleet, not the code.
    return "this problem requires machine_class '" + required +
           "'; this worker is '" +
           (machine_class_.empty() ? "unset" : machine_class_) + "'";
  }
  return "";
}

OrderOutcome OrderRunner::RunOrder(int slot, const proto::WorkOrder &order,
                                   const ProgressSink &progress) {
  OrderOutcome outcome;
  if (const std::string refusal = Refusal(order); !refusal.empty()) {
    outcome.result.set_error(refusal);
    return outcome;
  }

  sandbox_exec::Engine *engine = EngineFor(order);
  sandbox_exec::proto::Job job;
  std::string error;
  if (!JobForOrder(slot, order, config_, engine->capabilities(), &job,
                   &error)) {
    outcome.result.set_error(error);
    return outcome;
  }

  {
    std::lock_guard<std::mutex> lock(mutex_);
    running_[order.order_id()] = InFlight{job.id(), engine};
  }
  ProgressObserver observer(order.order_id(), &progress);
  const sandbox_exec::proto::JobResult result = engine->Run(job, &observer);
  {
    std::lock_guard<std::mutex> lock(mutex_);
    running_.erase(order.order_id());
  }

  return OutcomeFor(order, result);
}

void OrderRunner::Cancel(const std::string &order_id) {
  std::string job_id;
  sandbox_exec::Engine *engine = nullptr;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = running_.find(order_id);
    if (it == running_.end()) {
      return;
    }
    job_id = it->second.job_id;
    engine = it->second.engine;
  }
  engine->Cancel(job_id);
}

}  // namespace tournament_arena
