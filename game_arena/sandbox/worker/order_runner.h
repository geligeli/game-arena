#ifndef GAME_ARENA_GAME_ARENA_SANDBOX_WORKER_ORDER_RUNNER_H
#define GAME_ARENA_GAME_ARENA_SANDBOX_WORKER_ORDER_RUNNER_H

// One work order on one engine: the acceptance gates, and forwarding a cancel.

#include <functional>
#include <map>
#include <mutex>
#include <string>

#include "game_arena/proto/arena.pb.h"
#include "game_arena/sandbox/exec/engine.h"
#include "game_arena/sandbox/worker/order_job.h"
#include "game_arena/sandbox/worker/order_outcome.h"

namespace tournament_arena {

class OrderRunner {
 public:
  // Called with each phase the order reaches.
  using ProgressSink = std::function<void(const std::string &order_id,
                                          proto::OrderProgress::Phase phase)>;

  // Either may be null. An order's image, or its absence, picks the engine.
  OrderRunner(sandbox_exec::Engine *process_engine,
              sandbox_exec::Engine *container_engine, OrderJobConfig config,
              std::string machine_class = {});

  // Creates the per-slot directories up front.
  bool Warmup(int slots, std::string *error);

  OrderOutcome RunOrder(int slot, const proto::WorkOrder &order,
                        const ProgressSink &progress);

  // From the stream thread, while a slot thread is inside RunOrder.
  void Cancel(const std::string &order_id);

 private:
  // Non-empty when this worker must refuse |order| rather than run it.
  std::string Refusal(const proto::WorkOrder &order) const;
  // The engine this order runs on, or null when this worker has none for it.
  sandbox_exec::Engine *EngineFor(const proto::WorkOrder &order) const;

  struct InFlight {
    std::string job_id;
    sandbox_exec::Engine *engine = nullptr;
  };

  sandbox_exec::Engine *const process_engine_;    // may be null
  sandbox_exec::Engine *const container_engine_;  // may be null
  const OrderJobConfig config_;
  const std::string machine_class_;

  std::mutex mutex_;
  std::map<std::string, InFlight> running_;  // keyed by order id
};

}  // namespace tournament_arena

#endif  // GAME_ARENA_GAME_ARENA_SANDBOX_WORKER_ORDER_RUNNER_H
