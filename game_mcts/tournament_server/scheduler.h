#ifndef RISK_GAME_AI_CPP_TOURNAMENT_SERVER_SCHEDULER_H
#define RISK_GAME_AI_CPP_TOURNAMENT_SERVER_SCHEDULER_H

// Turns "rate this candidate" into work for the sandbox fleet.
//
// A job names a candidate and an opponent spec. The spec expands into one or
// more *batches*, and a batch is the unit of dispatch:
//
//   vs a built-in   one order  -- the worker's bot plays the server's builtin
//   vs a candidate  two orders -- both sides must be built and run, and each
//                                 dials the broker with
//                                 opponent="player:<the other>"
//
// A two-order batch is dispatched all at once, to however many workers have
// slots free. Dispatching half of one would leave the built bot parked at the
// broker's rendezvous until it times out, burning a slot and producing no
// game. That requirement is the whole reason batches exist.
//
// The scheduler owns no threads. Everything happens on the caller's thread
// under one mutex: enqueueing from an Arena RPC, and dispatching when a worker
// attaches, finishes an order, or drops.

#include <chrono>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "game_mcts/tournament_server/arena.pb.h"
#include "game_mcts/tournament_server/candidate_store.h"
#include "game_mcts/tournament_server/elo_store.h"
#include "game_mcts/tournament_server/fleet_worker.h"

namespace tournament_arena {

struct SchedulerConfig {
  // Where a built bot dials the broker. Not the fleet address: workers may run
  // on other hosts, so this must be an address they can reach.
  std::string broker_target = "localhost:50051";
  // Opponents a freshly submitted candidate is placed against.
  std::vector<std::string> placement_opponents = {"builtin:random",
                                                  "builtin:mcts"};
  int placement_games = 4;
  int default_games = 10;
  int max_games_per_job = 200;
  // How many rated rivals "ladder" spreads across.
  int ladder_size = 3;
  int build_timeout_s = 1800;
  int run_timeout_s = 1800;
};

class Scheduler {
 public:
  Scheduler(SchedulerConfig config, CandidateStore *candidates,
            tournament_broker::EloStore *elo_store);

  // --- agent side ------------------------------------------------------

  // Queues the placement series for a newly created candidate.
  auto EnqueuePlacement(const proto::Candidate &candidate) -> std::string;

  // Queues |games| games against |opponent|: "builtin:<spec>" |
  // "<candidate_id>" | "top" | "ladder". Returns nullopt with *error set on an
  // unusable request.
  auto EnqueueChallenge(const std::string &candidate_id,
                        const std::string &opponent, int games,
                        std::string *error) -> std::optional<std::string>;

  auto GetJob(const std::string &job_id) const -> std::optional<proto::Job>;

  // --- fleet side ------------------------------------------------------

  void AddWorker(std::shared_ptr<FleetWorker> worker);
  // Requeues whatever the worker had in flight, once.
  void RemoveWorker(const std::string &worker_id);
  void OnResult(const std::string &worker_id, const proto::OrderResult &result);

  // --- introspection (tests, /api) -------------------------------------

  auto worker_count() const -> int;
  auto queued_batches() const -> int;
  auto in_flight_orders() const -> int;

 private:
  struct Batch {
    std::vector<proto::WorkOrder> orders;
    bool dispatched = false;
    int outstanding = 0;
  };

  struct Job {
    proto::Job status;
    std::deque<Batch> pending;             // not yet dispatched
    std::map<std::string, Batch> running;  // keyed by the batch's first order
    // Orders whose result feeds this job's tally: the ones played by the job's
    // own candidate. The opponent's order exists only to make the game happen.
    std::map<std::string, bool> order_is_primary;
    bool aborted = false;
  };

  struct WorkerState {
    std::shared_ptr<FleetWorker> worker;
    std::vector<std::string> in_flight;  // order ids
  };

  // Caller holds mutex_.
  auto ExpandOpponentsLocked(const proto::Candidate &candidate,
                             const std::string &spec, std::string *error) const
      -> std::optional<std::vector<std::string>>;
  // Not const: each call consumes an order id, and the two orders of a paired
  // batch must not share one.
  auto MakeBatchLocked(const proto::Candidate &candidate,
                       const std::string &opponent,
                       int games) -> std::optional<Batch>;
  auto MakeOrderLocked(const proto::Candidate &candidate,
                       const std::string &opponent, int games,
                       const std::string &job_id) -> proto::WorkOrder;
  auto EnqueueLocked(const proto::Candidate &candidate,
                     const std::vector<std::string> &opponents,
                     int games) -> std::string;
  void DispatchLocked();
  void ConcludeJobLocked(Job *job);
  auto FreeSlotsLocked() const -> int;

  const SchedulerConfig config_;
  CandidateStore *candidates_;              // not owned
  tournament_broker::EloStore *elo_store_;  // not owned

  mutable std::mutex mutex_;
  std::map<std::string, Job> jobs_;
  std::deque<std::string> queue_;  // job ids with undispatched batches
  std::map<std::string, WorkerState> workers_;
  // order id -> (job id, worker id)
  std::map<std::string, std::pair<std::string, std::string>> order_owner_;
  uint64_t job_counter_ = 0;
  uint64_t order_counter_ = 0;
};

}  // namespace tournament_arena

#endif  // RISK_GAME_AI_CPP_TOURNAMENT_SERVER_SCHEDULER_H
