#ifndef GAME_ARENA_GAME_ARENA_SERVER_SCHEDULER_H
#define GAME_ARENA_GAME_ARENA_SERVER_SCHEDULER_H

// Turns "evaluate this candidate" into work for the sandbox fleet.
//
// A job names a candidate and an opponent spec; the spec expands into one or
// more orders, and an order is the unit of dispatch. **One order is one whole
// evaluation**: it carries both sides of a match, so a worker builds the
// candidate, builds its opponent (or names a builtin), referees the games and
// reports a tally, without any other worker cooperating.
//
// It used to take two mirrored orders that had to be dispatched together,
// because half a match was a built bot parked at a central broker's rendezvous
// burning a slot for no game. Moving the referee into the sandbox removed the
// pairing, and with it the batch, the all-or-nothing dispatch and the
// bookkeeping that told a job which half of a pair was its own.
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

#include "game_arena/proto/arena.pb.h"
#include "game_arena/proto/clients.pb.h"
#include "game_arena/server/candidate_store.h"
#include "game_arena/server/elo_store.h"
#include "game_arena/server/fleet_worker.h"
#include "game_arena/server/standings.h"

namespace tournament_arena {

struct SchedulerConfig {
  // The referee binary a worker starts per match, from
  // ProblemConfig.match.referee_target. Empty for a graded problem, which has
  // nothing to referee.
  std::string referee_target;
  // Wall-clock limit the referee gives one match, after which it reports what
  // was played. Kept under run_timeout_s so a stuck match yields a partial
  // tally rather than an order-level failure.
  int match_deadline_s = 1500;
  // From ProblemConfig.build.targets, still carrying "{submission_id}"; the
  // scheduler expands it per submission.
  std::vector<std::string> build_targets;
  // The target whose binary plays a match, likewise templated. Empty for a
  // graded problem, which runs a command rather than a bot.
  std::string bot_target;
  // A graded problem's command and how to fold its runs, templated the same
  // way. Absent for a match problem.
  std::optional<proto::GradeOrder> grade;
  // ProblemConfig.match.registry_options, stamped on every order and forwarded
  // to the referee untouched. The coordinator never reads these: what they
  // mean is known only to the registry linked into the referee.
  google::protobuf::Map<std::string, std::string> registry_options;
  // Stamped on every order: the problem, not the worker, knows whether its
  // submissions may run arbitrary code at build time.
  bool require_container = false;
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
  // |standings| is where a finished order's result lands. The coordinator is
  // the only thing that writes standings: a referee's own ratings die with its
  // container.
  Scheduler(SchedulerConfig config, CandidateStore *candidates,
            tournament_broker::EloStore *elo_store, Standings *standings);

  // A held quota slot.
  //
  // It counts against the client's limit from the moment it is granted, which
  // is the whole point: two concurrent submits from one client must not both
  // pass, and a check that is not part of the same locked step as the claim
  // cannot promise that.
  //
  // Granting it before the submission is stored also means a refused submit
  // stores nothing. Check-then-create-then-enqueue would leave a submission on
  // disk that the quota then rejects.
  class Reservation {
   public:
    Reservation() = default;
    ~Reservation();
    Reservation(Reservation &&other) noexcept;
    auto operator=(Reservation &&other) noexcept -> Reservation &;
    Reservation(const Reservation &) = delete;
    auto operator=(const Reservation &) -> Reservation & = delete;

    auto client_id() const -> const std::string & { return client_id_; }
    // Job ids aborted to make room, when the caller asked to replace.
    auto superseded() const -> const std::vector<std::string> & {
      return superseded_;
    }

   private:
    friend class Scheduler;
    Scheduler *scheduler_ = nullptr;
    std::string client_id_;
    std::vector<std::string> superseded_;
  };

  // --- agent side ------------------------------------------------------

  // Claims a quota slot for |client_id|, or returns nullopt with *error
  // explaining which limit was hit. |cancel_running| aborts the client's
  // in-flight jobs to make room instead of refusing.
  //
  // An empty |client_id| means the server is running without a client
  // registry: nothing to meter, and the reservation is granted.
  auto TryReserve(const std::string &client_id, const proto::ClientQuota &quota,
                  bool cancel_running,
                  std::string *error) -> std::optional<Reservation>;

  // Queues the placement series for a newly created candidate, consuming
  // |reservation|.
  auto EnqueuePlacement(const proto::Candidate &candidate,
                        Reservation reservation) -> std::string;

  // Queues |games| games against |opponent|: "builtin:<spec>" |
  // "<candidate_id>" | "top" | "ladder". Returns nullopt with *error set on an
  // unusable request.
  auto EnqueueChallenge(const std::string &candidate_id,
                        const std::string &opponent, int games,
                        Reservation reservation,
                        std::string *error) -> std::optional<std::string>;

  // Queues another measurement of |candidate_id| for a graded problem.
  // |repeats| <= 0 uses the problem's default. Returns nullopt with *error set.
  auto EnqueueRegrade(const std::string &candidate_id, int repeats,
                      Reservation reservation,
                      std::string *error) -> std::optional<std::string>;

  auto GetJob(const std::string &job_id) const -> std::optional<proto::Job>;

  // --- fleet side ------------------------------------------------------

  void AddWorker(std::shared_ptr<FleetWorker> worker);
  // Requeues whatever the worker had in flight, once.
  void RemoveWorker(const std::string &worker_id);
  void OnResult(const std::string &worker_id, const proto::OrderResult &result);

  // --- introspection (tests, /api) -------------------------------------

  auto worker_count() const -> int;
  auto queued_orders() const -> int;
  auto in_flight_orders() const -> int;

 private:
  struct Job {
    proto::Job status;
    // Who this job is metered against. Empty when no registry is configured.
    std::string client_id;
    std::deque<proto::WorkOrder> pending;             // not yet dispatched
    std::map<std::string, proto::WorkOrder> running;  // keyed by order id
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
  // Builds the whole order, opponent sources included. Returns nullopt when the
  // named rival cannot play (unknown, not ready, wrong game). Not const: each
  // call consumes an order id.
  auto MakeOrderLocked(
      const proto::Candidate &candidate, const std::string &opponent, int games,
      const std::string &job_id) -> std::optional<proto::WorkOrder>;
  // Fills one side of an order: its patch and its expanded bazel targets.
  // Returns false when the patch cannot be read, which makes the order
  // unrunnable rather than silently short.
  auto FillSideLocked(const proto::Candidate &candidate,
                      proto::Side *side) const -> bool;
  auto EnqueueLocked(const proto::Candidate &candidate,
                     const std::vector<std::string> &opponents, int games,
                     const std::string &client_id) -> std::string;
  // Aborts |job|, cancelling whatever it has in flight. Caller holds mutex_.
  void AbortJobLocked(Job *job, const std::string &reason);
  // Drops a reservation that was never consumed. Called by ~Reservation.
  void ReleaseReservationLocked(const std::string &client_id);
  void DispatchLocked();
  void ConcludeJobLocked(Job *job);
  auto FreeSlotsLocked() const -> int;

  const SchedulerConfig config_;
  CandidateStore *candidates_;              // not owned
  tournament_broker::EloStore *elo_store_;  // not owned
  Standings *standings_;                    // not owned

  mutable std::mutex mutex_;
  std::map<std::string, Job> jobs_;
  std::deque<std::string> queue_;  // job ids with undispatched batches
  std::map<std::string, WorkerState> workers_;
  // order id -> (job id, worker id)
  std::map<std::string, std::pair<std::string, std::string>> order_owner_;
  // client id -> slots granted by TryReserve but not yet turned into a job.
  // Without this, two concurrent submits both see zero running jobs and both
  // pass.
  std::map<std::string, int> reserved_;
  uint64_t job_counter_ = 0;
  uint64_t order_counter_ = 0;
};

}  // namespace tournament_arena

#endif  // GAME_ARENA_GAME_ARENA_SERVER_SCHEDULER_H
