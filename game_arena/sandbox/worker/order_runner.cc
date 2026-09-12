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

OrderRunner::OrderRunner(sandbox_exec::Engine *engine, OrderJobConfig config,
                         std::string machine_class)
    : engine_(engine),
      config_(std::move(config)),
      machine_class_(std::move(machine_class)) {}

auto OrderRunner::Warmup(int slots, std::string *error) -> bool {
  if (engine_->capabilities().isolates) {
    // A container mounts the repo, so it has to be a path on this host rather
    // than a URL to clone from.
    if (!std::filesystem::is_directory(config_.source_repo)) {
      *error = "--repo '" + config_.source_repo +
               "' is not a directory; the docker backend needs a local path, "
               "because it is mounted into the containers";
      return false;
    }
    if (!std::filesystem::exists(std::filesystem::path(config_.source_repo) /
                                 ".git")) {
      *error = "--repo '" + config_.source_repo +
               "' is not a git repository; the backend checks candidates out "
               "at a commit";
      return false;
    }
  }
  std::error_code ec;
  // Bind-mount sources must exist before docker will accept them.
  std::filesystem::create_directories(config_.disk_cache, ec);
  for (int slot = 0; slot < slots; ++slot) {
    const std::filesystem::path slot_dir =
        config_.work_dir / ("slot" + std::to_string(slot));
    std::filesystem::create_directories(slot_dir / "overlay", ec);
    std::filesystem::create_directories(slot_dir / "bazel_output_base", ec);
    if (ec) {
      *error = "cannot create slot directories under " +
               config_.work_dir.string() + ": " + ec.message();
      return false;
    }
    if (!sandbox_exec::EnsureClone(config_.git, config_.source_repo,
                                   slot_dir / "repo", SlotLogDir(config_, slot),
                                   error)) {
      return false;
    }
  }
  return true;
}

auto OrderRunner::Refusal(const proto::WorkOrder &order) const -> std::string {
  if (order.require_container() && !engine_->capabilities().isolates) {
    // The problem said its submissions need a container, and this engine is
    // not one: a submitted genrule here runs as this worker's own user.
    // Better to hand the order back than to quietly run it.
    return "this problem requires a container; run the worker with "
           "--backend=docker";
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

  sandbox_exec::proto::Job job;
  std::string error;
  if (!JobForOrder(slot, order, config_, engine_->capabilities(), &job,
                   &error)) {
    outcome.error = error;
    return outcome;
  }

  {
    std::lock_guard<std::mutex> lock(mutex_);
    job_ids_[order.order_id()] = job.id();
  }
  ProgressObserver observer(order.order_id(), &progress);
  const sandbox_exec::proto::JobResult result = engine_->Run(job, &observer);
  {
    std::lock_guard<std::mutex> lock(mutex_);
    job_ids_.erase(order.order_id());
  }

  return OutcomeFor(order, result);
}

void OrderRunner::Cancel(const std::string &order_id) {
  std::string job_id;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = job_ids_.find(order_id);
    if (it == job_ids_.end()) {
      return;
    }
    job_id = it->second;
  }
  engine_->Cancel(job_id);
}

}  // namespace tournament_arena
