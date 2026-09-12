#include "game_arena/sandbox/worker/order_runner.h"

#include <filesystem>
#include <string>
#include <system_error>
#include <utility>

#include "game_arena/sandbox/exec/checkout.h"
#include "game_arena/sandbox/exec/sandbox_job.pb.h"

namespace tournament_arena {

namespace {

// Maps the engine's phases onto the arena's, which is the whole reason the
// engine reports phases by name rather than by enum: it does not know that
// "build" is a build.
class ProgressObserver final : public sandbox_exec::Observer {
 public:
  ProgressObserver(std::string order_id, const OrderRunner::ProgressSink *sink)
      : order_id_(std::move(order_id)), sink_(sink) {}

  void OnWorkspaceReady(const std::string &) override {
    Report(proto::OrderProgress::CLONING);
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

auto OrderRunner::Warmup(int slots, std::string *error) -> bool {
  // Only the directories now. The tree itself arrives with the order -- a
  // worker has no repository of its own -- so the clone happens in the
  // engine's PrepareWorkspace, which is idempotent and therefore pays for
  // itself once per slot rather than once per order.
  //
  // These still exist up front because docker conjures a missing bind-mount
  // source up as an empty directory owned by root, which is a confusing way
  // to find out the path was wrong.
  std::error_code ec;
  std::filesystem::create_directories(config_.disk_cache, ec);
  for (int slot = 0; slot < slots; ++slot) {
    const std::filesystem::path slot_dir =
        config_.work_dir / ("slot" + std::to_string(slot));
    std::filesystem::create_directories(slot_dir / "overlay", ec);
    std::filesystem::create_directories(slot_dir / "bazel_output_base", ec);
    std::filesystem::create_directories(SlotLogDir(config_, slot), ec);
    if (ec) {
      *error = "cannot create slot directories under " +
               config_.work_dir.string() + ": " + ec.message();
      return false;
    }
  }
  return true;
}

auto OrderRunner::engines() const -> std::string {
  if (process_engine_ != nullptr && container_engine_ != nullptr) {
    return process_engine_->name() + "+" + container_engine_->name();
  }
  if (container_engine_ != nullptr) {
    return container_engine_->name();
  }
  return process_engine_ != nullptr ? process_engine_->name() : "none";
}

auto OrderRunner::EngineFor(const proto::WorkOrder &order) const
    -> sandbox_exec::Engine * {
  // The problem decides, by naming an image or not. The coordinator already
  // refuses a problem that sets require_container without one
  // (server/problem_config.cc), so "needs a container" and "named an image"
  // cannot disagree.
  return order.sandbox().image().empty() ? process_engine_ : container_engine_;
}

auto OrderRunner::Refusal(const proto::WorkOrder &order) const -> std::string {
  const sandbox_exec::Engine *engine = EngineFor(order);
  if (engine == nullptr) {
    return order.sandbox().image().empty()
               ? "this worker cannot run unsandboxed orders"
               : "this problem needs a container and this worker has no "
                 "container engine";
  }
  if (order.require_container() && !engine->capabilities().isolates) {
    // The problem said its submissions need a container and this engine is
    // not one: a submitted genrule here would run as this worker's own user.
    // Better to hand the order back than to quietly run it.
    return "this problem requires a container, and this order named no image "
           "to run one from";
  }
  const std::string &required = order.grade().require_machine_class();
  if (!required.empty() && required != machine_class_) {
    // A wall-clock number from the wrong kind of host is worse than no
    // number: it looks like a result. This is what stops a leaderboard from
    // ranking the fleet instead of the submissions.
    return "this problem requires machine_class '" + required +
           "'; this worker is '" +
           (machine_class_.empty() ? "unset" : machine_class_) + "'";
  }
  return "";
}

auto OrderRunner::RunOrder(int slot, const proto::WorkOrder &order,
                           const ProgressSink &progress) -> OrderOutcome {
  OrderOutcome outcome;
  if (const std::string refusal = Refusal(order); !refusal.empty()) {
    outcome.error = refusal;
    return outcome;
  }

  sandbox_exec::Engine *engine = EngineFor(order);
  sandbox_exec::proto::Job job;
  std::string error;
  if (!JobForOrder(slot, order, config_, engine->capabilities(), &job,
                   &error)) {
    outcome.error = error;
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
