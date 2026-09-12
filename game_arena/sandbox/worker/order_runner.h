#ifndef GAME_ARENA_GAME_ARENA_SANDBOX_WORKER_ORDER_RUNNER_H
#define GAME_ARENA_GAME_ARENA_SANDBOX_WORKER_ORDER_RUNNER_H

// Running one work order on one engine.
//
// What is left of the two backends once the mechanics moved into
// //game_arena/sandbox/exec and the translation into order_job.h: the
// acceptance gates, and forwarding a cancel. The worker loop knows only this,
// so which engine is underneath does not touch scheduling, reporting or the
// fleet protocol.

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
  // |progress| is called with each phase the order reaches, so the worker can
  // report it without the engine knowing what a phase means to the arena.
  using ProgressSink = std::function<void(const std::string &order_id,
                                          proto::OrderProgress::Phase phase)>;

  // Either engine may be null: a host with no docker passes only a process
  // engine, and an order that needs a container is then refused rather than
  // quietly run unsandboxed. Which one an order gets is the *problem's*
  // decision -- it names an image or it does not -- not a worker flag.
  OrderRunner(sandbox_exec::Engine *process_engine,
              sandbox_exec::Engine *container_engine, OrderJobConfig config,
              std::string machine_class = {});

  // Prepares per-slot state up front, so the first order does not pay for
  // it: one clone per slot, and the bind-mount sources docker would
  // otherwise conjure up as empty directories. Here rather than on the
  // engine because a slot is an arena concept -- the engine is handed paths,
  // it does not know how many of them there will be.
  auto Warmup(int slots, std::string *error) -> bool;

  auto RunOrder(int slot, const proto::WorkOrder &order,
                const ProgressSink &progress) -> OrderOutcome;

  // Aborts |order_id| if this runner is running it. Called from the stream
  // thread while a slot thread is inside RunOrder.
  void Cancel(const std::string &order_id);

  // What this worker can run, for the hello. Both engines when it has both.
  auto engines() const -> std::string;

 private:
  // Why this worker will not run |order| at all, or empty if it will. Refused
  // rather than attempted: an order run on the wrong kind of host produces a
  // number that looks like a result.
  auto Refusal(const proto::WorkOrder &order) const -> std::string;
  // The engine this order runs on, or null when this worker has none for it.
  auto EngineFor(const proto::WorkOrder &order) const -> sandbox_exec::Engine *;

  // What a Cancel needs: which job, and which engine took it.
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
