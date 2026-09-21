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
#include "game_arena/server/fleet_worker.h"
#include "game_arena/standings/elo_store.h"
#include "game_arena/standings/standings.h"

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
  // The rest of the sandbox -- its image is where the tree is -- and the
  // build's extra flags. All on every order because a worker has none of its
  // own to disagree with: two submissions are only comparable if they were
  // built the same way.
  proto::SandboxOrder sandbox;
  std::vector<std::string> bazel_flags;
  // How the referee bounds a game. Forwarded rather than left to the
  // referee's own flag defaults, which is what used to happen.
  int turn_timeout_ms = 0;
  int game_time_budget_ms = 0;
  int max_moves_per_game = 0;
  // Opponents a freshly submitted candidate is placed against.
  std::vector<std::string> placement_opponents = {"builtin:random",
                                                  "builtin:mcts"};
  int placement_games = 4;
  // How many rated rivals a placement also plays, spread across the board.
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
            Standings *standings);

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
    Reservation &operator=(Reservation &&other) noexcept;
    Reservation(const Reservation &) = delete;
    Reservation &operator=(const Reservation &) = delete;

    const std::string &client_id() const { return client_id_; }
    // Job ids aborted to make room, when the caller asked to replace.
    const std::vector<std::string> &superseded() const { return superseded_; }

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
  std::optional<Reservation> TryReserve(const std::string &client_id,
                                        const proto::ClientQuota &quota,
                                        bool cancel_running,
                                        std::string *error);

  // Queues the placement series for a newly created candidate -- the
  // problem's placement opponents, then a ladder of rated rivals -- consuming
  // |reservation|.
  std::string EnqueuePlacement(const proto::Candidate &candidate,
                               Reservation reservation);

  std::optional<proto::Job> GetJob(const std::string &job_id) const;

  // Records how far |progress|'s order has got, so GetJob can say more than
  // "running". Unknown orders are ignored: a progress report racing the
  // result that retired the order is normal, not an error.
  void OnProgress(const proto::OrderProgress &progress);

  // --- fleet side ------------------------------------------------------

  void AddWorker(std::shared_ptr<FleetWorker> worker);
  // Requeues whatever the worker had in flight, once.
  void RemoveWorker(const std::string &worker_id);
  void OnResult(const std::string &worker_id, const proto::OrderResult &result);

  // --- introspection (tests, /api) -------------------------------------

  int worker_count() const;
  int queued_orders() const;
  int in_flight_orders() const;

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
  std::vector<std::string> LadderLocked(const std::string &self) const;
  // Builds the whole order, opponent sources included. Returns nullopt when the
  // named rival cannot play (unknown, not ready, wrong game). Not const: each
  // call consumes an order id.
  std::optional<proto::WorkOrder> MakeOrderLocked(
      const proto::Candidate &candidate, const std::string &opponent, int games,
      const std::string &job_id);
  // Fills one side of an order: its patch and its expanded bazel targets.
  // Returns false when the patch cannot be read, which makes the order
  // unrunnable rather than silently short.
  bool FillSideLocked(const proto::Candidate &candidate,
                      proto::Side *side) const;
  std::string EnqueueLocked(const proto::Candidate &candidate,
                            const std::vector<std::string> &opponents,
                            int games, const std::string &client_id);
  // Aborts |job|, cancelling whatever it has in flight. Caller holds mutex_.
  void AbortJobLocked(Job *job, const std::string &reason);
  // Drops a reservation that was never consumed. Called by ~Reservation.
  void ReleaseReservationLocked(const std::string &client_id);
  void DispatchLocked();
  void ConcludeJobLocked(Job *job);
  int FreeSlotsLocked() const;

  const SchedulerConfig config_;
  CandidateStore *candidates_;  // not owned
  Standings *standings_;        // not owned

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
