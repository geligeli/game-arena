#ifndef GAME_ARENA_GAME_ARENA_SERVER_SCHEDULER_H
#define GAME_ARENA_GAME_ARENA_SERVER_SCHEDULER_H

// No threads of its own: everything runs on the caller's, under one mutex.

#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
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
#include "game_arena/standings/game_history.h"
#include "game_arena/standings/standings.h"

namespace tournament_arena {

class Scheduler {
 public:
  // |history| and |job_log| may be null. |on_concluded| gets each job that
  // runs to DONE or FAILED, under the scheduler's lock.
  Scheduler(SchedulerConfig config, CandidateStore *candidates,
            Standings *standings,
            tournament_broker::GameHistory *history = nullptr,
            JobLog *job_log = nullptr,
            std::function<void(const proto::Job &)> on_concluded = {});

  // Counted when granted, so two concurrent submits cannot both pass.
  class Reservation {
   public:
    const std::string &client_id() const {
      return held_.get_deleter().client_id;
    }
    // Job ids aborted to make room, when the caller asked to replace.
    const std::vector<std::string> &superseded() const { return superseded_; }

   private:
    friend class Scheduler;
    // Hands the slot back: the submission was rejected or failed to store.
    struct Release {
      std::string client_id;
      void operator()(Scheduler *scheduler) const;
    };
    // Null when there is nothing to release: no registry, or consumed.
    std::unique_ptr<Scheduler, Release> held_;
    std::vector<std::string> superseded_;
  };

  // |cancel_running| aborts the client's jobs instead of refusing.
  std::optional<Reservation> TryReserve(const std::string &client_id,
                                        const proto::ClientQuota &quota,
                                        bool cancel_running,
                                        std::string *error);

  // The problem's placement opponents, then a ladder of rated rivals.
  std::string EnqueuePlacement(const proto::Candidate &candidate,
                               Reservation reservation);

  // One match of |games| against |opponent|, metered against no one. Either
  // side may be a builtin, as "builtin:<spec>" for |candidate|'s id.
  std::string EnqueueMatch(const proto::Candidate &candidate,
                           const std::string &opponent, int games);

  std::optional<proto::Job> GetJob(const std::string &job_id) const;

  // Unknown orders are ignored: progress can race the result that retired one.
  void OnProgress(const proto::OrderProgress &progress);

  void AddWorker(std::shared_ptr<FleetWorker> worker);
  // Requeues whatever the worker had in flight, once.
  void RemoveWorker(const std::string &worker_id);
  void OnResult(const std::string &worker_id, const proto::OrderResult &result);
  // A retired order's games are dropped, as its result would be.
  void OnGame(const proto::OrderGame &game);

  int worker_count() const;
  // Every attached worker's slots: what the fleet can run at once.
  int total_slots() const;
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
    // Cleared once the job has finished and is on disk.
    JobRecord record;
  };

  struct WorkerState {
    std::shared_ptr<FleetWorker> worker;
    std::vector<std::string> in_flight;  // order ids
  };

  // Rated rivals for |candidate|: never its author's other versions.
  std::vector<std::string> LadderLocked(
      const proto::Candidate &candidate) const;
  // Nullopt when the rival cannot play. Not const: it consumes an order id.
  std::optional<proto::WorkOrder> MakeOrderLocked(
      const proto::Candidate &candidate, const std::string &opponent, int games,
      const std::string &job_id);
  void FillSideLocked(const proto::Candidate &candidate,
                      proto::Side *side) const;
  // |record_patch|: keep the submission's patch in the job log, which only
  // a submission needs; a match's candidate is already in the store.
  std::string EnqueueLocked(const proto::Candidate &candidate,
                            const std::vector<std::string> &opponents,
                            int games, const std::string &client_id,
                            bool record_patch = true);
  void AbortJobLocked(Job *job, const std::string &reason);
  void ReleaseReservationLocked(const std::string &client_id);
  void DispatchLocked();
  void ConcludeJobLocked(Job *job);
  void PersistLocked(Job *job);

  const SchedulerConfig config_;
  CandidateStore *candidates_;               // not owned
  Standings *standings_;                     // not owned
  tournament_broker::GameHistory *history_;  // not owned
  JobLog *job_log_;                          // not owned
  const std::function<void(const proto::Job &)> on_concluded_;

  mutable std::mutex mutex_;
  std::map<std::string, Job> jobs_;
  std::deque<std::string> queue_;  // job ids with undispatched orders
  std::map<std::string, WorkerState> workers_;
  // order id -> (job id, worker id)
  std::map<std::string, std::pair<std::string, std::string>> order_owner_;
  // client id -> slots granted by TryReserve but not yet turned into a job.
  std::map<std::string, int> reserved_;
  uint64_t job_counter_ = 0;
  uint64_t order_counter_ = 0;
};

}  // namespace tournament_arena

#endif  // GAME_ARENA_GAME_ARENA_SERVER_SCHEDULER_H
