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
#include "game_arena/server/job_log.h"
#include "game_arena/server/scheduler_config.pb.h"
#include "game_arena/standings/elo_store.h"
#include "game_arena/standings/game_history.h"
#include "game_arena/standings/standings.h"

namespace tournament_arena {

class Scheduler {
 public:
  // |standings| is where a finished order's result lands. The coordinator is
  // the only thing that writes standings: a referee's own ratings die with its
  // container. |history|, when set, keeps the games a running order played,
  // and |job_log| every job: its submission and each order's result.
  Scheduler(SchedulerConfig config, CandidateStore *candidates,
            Standings *standings,
            tournament_broker::GameHistory *history = nullptr,
            JobLog *job_log = nullptr);

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
  // Keeps one game a running order played. A retired order's are dropped, as
  // its result would be.
  void OnGame(const proto::OrderGame &game);

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
    // What job_log_ keeps. Cleared once the job has finished and is on disk.
    JobRecord record;
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
  // Writes |job|'s record to job_log_. Caller holds mutex_.
  void PersistLocked(Job *job);
  int FreeSlotsLocked() const;

  const SchedulerConfig config_;
  CandidateStore *candidates_;               // not owned
  Standings *standings_;                     // not owned
  tournament_broker::GameHistory *history_;  // not owned
  JobLog *job_log_;                          // not owned

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
