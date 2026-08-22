#ifndef RISK_GAME_AI_CPP_TOURNAMENT_SERVER_SANDBOX_WORKER_SANDBOX_BACKEND_H
#define RISK_GAME_AI_CPP_TOURNAMENT_SERVER_SANDBOX_WORKER_SANDBOX_BACKEND_H

// How a work order is actually executed: check out the tree, patch the
// candidate in, build it, run the bot against the broker.
//
// One interface, two intended implementations. `local` runs the steps as
// subprocesses with resource limits and timeouts -- enough to stop a runaway
// candidate, not a security boundary. `docker` will run the same steps inside
// a container. The worker loop knows only this interface, so adding the
// second backend does not touch scheduling, reporting or the fleet protocol.

#include <string>

#include "game_mcts/tournament_server/arena.pb.h"

namespace tournament_arena {

struct OrderOutcome {
  bool build_ok = false;
  // Already compacted: full bazel logs never leave the worker.
  std::string build_log;
  int games_played = 0;
  int wins = 0;
  int draws = 0;
  int losses = 0;
  double elo = 0.0;
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
};

}  // namespace tournament_arena

#endif  // RISK_GAME_AI_CPP_TOURNAMENT_SERVER_SANDBOX_WORKER_SANDBOX_BACKEND_H
