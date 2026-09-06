#ifndef GAME_ARENA_GAME_ARENA_SANDBOX_WORKER_SANDBOX_BACKEND_H
#define GAME_ARENA_GAME_ARENA_SANDBOX_WORKER_SANDBOX_BACKEND_H

// How a work order is actually executed: check out the tree, patch the
// candidate in, build it, run the bot against the broker.
//
// One interface, two implementations. `local` runs the steps as subprocesses
// with resource limits and timeouts -- enough to stop a runaway candidate,
// not a security boundary. `docker` runs the same steps in throwaway
// containers over a per-slot overlay (see docker_backend.h). The worker loop
// knows only this interface, so a backend swap does not touch scheduling,
// reporting or the fleet protocol.

#include <map>
#include <string>

#include "game_arena/proto/arena.pb.h"

namespace tournament_arena {

struct OrderOutcome {
  bool build_ok = false;
  // Already compacted: full bazel logs never leave the worker.
  std::string build_log;
  // Whose build broke, when build_ok is false. An order builds both sides of a
  // match, and the opponent failing to compile is not the submitter's fault --
  // without this the coordinator would retire the wrong submission. Empty means
  // the order's own candidate.
  std::string build_failed_candidate_id;
  // Games for a match order, measurement runs for a graded one.
  int games_played = 0;
  int wins = 0;
  int draws = 0;
  int losses = 0;
  double elo = 0.0;
  // What a graded order measured, already aggregated across runs and filtered
  // to the metrics the problem ranks on. Empty for a match order.
  std::map<std::string, double> metrics;
  // Non-empty when the order could not be completed at all -- checkout failed,
  // the bot crashed, a step timed out. Distinct from a clean build that simply
  // lost every game.
  std::string error;
};

class SandboxBackend {
 public:
  virtual ~SandboxBackend() = default;

  // Runs |order| to completion. |slot| identifies which of the worker's
  // parallel workspaces to use, so concurrent orders never share a checkout or
  // a bazel output base. Called from the slot's own thread.
  virtual auto RunOrder(int slot,
                        const proto::WorkOrder &order) -> OrderOutcome = 0;

  virtual auto name() const -> std::string = 0;

  // Prepares per-slot state up front, so the first order does not pay for it.
  // Default: the backend has no per-slot state to prepare. Returns false with
  // *error set when the worker cannot serve orders at all.
  virtual auto Warmup(int slots, std::string *error) -> bool {
    (void)slots;
    (void)error;
    return true;
  }

  // Aborts |order_id| if this backend is running it. Called from the stream
  // thread while a slot thread is inside RunOrder, so an implementation must be
  // safe against the order finishing concurrently -- killing something that has
  // already exited is a no-op, and that is the race worth designing for rather
  // than locking against.
  //
  // Default: nothing to abort. Only a backend that can actually stop work
  // mid-flight should override, because the arena treats a silent no-op as
  // "the order will finish on its own", which is true here.
  virtual void Cancel(const std::string &order_id) { (void)order_id; }
};

}  // namespace tournament_arena

#endif  // GAME_ARENA_GAME_ARENA_SANDBOX_WORKER_SANDBOX_BACKEND_H
