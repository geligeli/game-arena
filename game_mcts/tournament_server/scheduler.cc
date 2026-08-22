#include "game_mcts/tournament_server/scheduler.h"

#include <algorithm>
#include <utility>

#include "absl/log/log.h"

namespace tournament_arena {

namespace {

auto NowUnixMs() -> int64_t {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

constexpr std::string_view kBuiltinPrefix = "builtin:";

auto IsBuiltin(const std::string &opponent) -> bool {
  return opponent.rfind(kBuiltinPrefix, 0) == 0;
}

}  // namespace

Scheduler::Scheduler(SchedulerConfig config, CandidateStore *candidates,
                     tournament_broker::EloStore *elo_store)
    : config_(std::move(config)),
      candidates_(candidates),
      elo_store_(elo_store) {}

auto Scheduler::MakeOrderLocked(const proto::Candidate &candidate,
                                const std::string &opponent, int games,
                                const std::string &job_id)
    -> proto::WorkOrder {
  proto::WorkOrder order;
  // Per order, not per opponent: the two sides of a paired batch are tracked
  // separately everywhere downstream, so they must not share an id.
  order.set_order_id("o" + std::to_string(NowUnixMs()) + "_" +
                     std::to_string(++order_counter_));
  order.set_job_id(job_id);
  order.set_candidate_id(candidate.candidate_id());
  order.set_game(candidate.game());
  order.set_base_commit(candidate.base_commit());
  order.set_entry_header(candidate.entry_header());
  *order.mutable_extra_deps() = candidate.extra_deps();
  *order.mutable_params() = candidate.params();
  order.set_broker_target(config_.broker_target);
  order.set_opponent(opponent);
  order.set_num_games(games);
  order.set_build_timeout_s(config_.build_timeout_s);
  order.set_run_timeout_s(config_.run_timeout_s);

  // Sources travel with the order so a worker needs nothing but the repo and
  // this message -- no callback to the arena, no shared filesystem.
  std::string error;
  for (const std::string &path : candidate.file_paths()) {
    const auto content =
        candidates_->ReadSource(candidate.candidate_id(), path, &error);
    if (!content.has_value()) {
      LOG(ERROR) << "Candidate " << candidate.candidate_id()
                 << ": cannot read '" << path << "': " << error;
      continue;
    }
    auto *file = order.add_files();
    file->set_path(path);
    file->set_content(*content);
  }
  return order;
}

auto Scheduler::MakeBatchLocked(const proto::Candidate &candidate,
                                const std::string &opponent, int games)
    -> std::optional<Batch> {
  Batch batch;
  if (IsBuiltin(opponent)) {
    batch.orders.push_back(
        MakeOrderLocked(candidate, opponent, games, /*job_id=*/""));
    return batch;
  }

  const auto rival = candidates_->Get(opponent);
  if (!rival.has_value() || rival->status() != proto::Candidate::READY) {
    return std::nullopt;
  }
  // Both sides get built and run, each naming the other. The broker pairs them
  // when the second one arrives.
  batch.orders.push_back(MakeOrderLocked(
      candidate, "player:" + rival->candidate_id(), games, ""));
  batch.orders.push_back(MakeOrderLocked(
      *rival, "player:" + candidate.candidate_id(), games, ""));
  return batch;
}

auto Scheduler::ExpandOpponentsLocked(const proto::Candidate &candidate,
                                      const std::string &spec,
                                      std::string *error) const
    -> std::optional<std::vector<std::string>> {
  if (spec.empty()) {
    *error = "opponent is required";
    return std::nullopt;
  }
  if (IsBuiltin(spec)) {
    return std::vector<std::string>{spec};
  }
  if (spec != "top" && spec != "ladder") {
    const auto rival = candidates_->Get(spec);
    if (!rival.has_value()) {
      *error = "unknown opponent '" + spec +
               "' (expected builtin:<spec>, a candidate id, \"top\" or "
               "\"ladder\")";
      return std::nullopt;
    }
    if (rival->candidate_id() == candidate.candidate_id()) {
      *error = "a candidate cannot play itself";
      return std::nullopt;
    }
    if (rival->status() != proto::Candidate::READY) {
      *error = "opponent '" + spec + "' is not ready to play";
      return std::nullopt;
    }
    if (rival->game() != candidate.game()) {
      *error = "opponent '" + spec + "' plays " + rival->game() + ", not " +
               candidate.game();
      return std::nullopt;
    }
    return std::vector<std::string>{spec};
  }

  // "top" and "ladder" are resolved now, against the current standings, rather
  // than at dispatch: an agent asking to be measured wants the field as it
  // stood when it asked.
  std::vector<std::pair<double, std::string>> rated;
  for (const proto::Candidate &other : candidates_->List()) {
    if (other.candidate_id() == candidate.candidate_id() ||
        other.game() != candidate.game() ||
        other.status() != proto::Candidate::READY) {
      continue;
    }
    const auto rating =
        elo_store_->Get(other.game(), other.candidate_id());
    rated.emplace_back(rating.elo(), other.candidate_id());
  }
  if (rated.empty()) {
    *error = "no rated opponent is available yet for " + candidate.game();
    return std::nullopt;
  }
  std::sort(rated.begin(), rated.end(),
            [](const auto &a, const auto &b) { return a.first > b.first; });

  std::vector<std::string> opponents;
  const int wanted = spec == "top" ? 1 : config_.ladder_size;
  for (const auto &[elo, id] : rated) {
    if (static_cast<int>(opponents.size()) >= wanted) {
      break;
    }
    opponents.push_back(id);
  }
  return opponents;
}

auto Scheduler::EnqueueLocked(const proto::Candidate &candidate,
                              const std::vector<std::string> &opponents,
                              int games) -> std::string {
  const std::string job_id =
      "j" + std::to_string(NowUnixMs()) + "_" + std::to_string(++job_counter_);

  Job job;
  job.status.set_job_id(job_id);
  job.status.set_candidate_id(candidate.candidate_id());
  job.status.set_state(proto::Job::QUEUED);
  job.status.set_created_unix_ms(NowUnixMs());

  int requested = 0;
  for (const std::string &opponent : opponents) {
    auto batch = MakeBatchLocked(candidate, opponent, games);
    if (!batch.has_value()) {
      LOG(WARNING) << "Job " << job_id << ": skipping opponent '" << opponent
                   << "' (not runnable)";
      continue;
    }
    for (proto::WorkOrder &order : batch->orders) {
      order.set_job_id(job_id);
      job.order_is_primary[order.order_id()] =
          order.candidate_id() == candidate.candidate_id();
    }
    batch->outstanding = static_cast<int>(batch->orders.size());
    requested += games;
    job.pending.push_back(std::move(*batch));
  }
  job.status.set_games_requested(requested);

  jobs_[job_id] = std::move(job);
  if (jobs_[job_id].pending.empty()) {
    // Nothing runnable; do not leave the caller polling a job that will never
    // move.
    jobs_[job_id].status.set_state(proto::Job::FAILED);
    jobs_[job_id].status.set_error("no runnable opponent");
    jobs_[job_id].status.set_finished_unix_ms(NowUnixMs());
    return job_id;
  }
  queue_.push_back(job_id);
  DispatchLocked();
  return job_id;
}

auto Scheduler::EnqueuePlacement(const proto::Candidate &candidate)
    -> std::string {
  std::lock_guard lock(mutex_);
  return EnqueueLocked(candidate, config_.placement_opponents,
                       config_.placement_games);
}

auto Scheduler::EnqueueChallenge(const std::string &candidate_id,
                                 const std::string &opponent, int games,
                                 std::string *error)
    -> std::optional<std::string> {
  std::lock_guard lock(mutex_);
  const auto candidate = candidates_->Get(candidate_id);
  if (!candidate.has_value()) {
    *error = "unknown candidate '" + candidate_id + "'";
    return std::nullopt;
  }
  if (candidate->status() == proto::Candidate::BUILD_FAILED) {
    *error = "candidate '" + candidate_id + "' failed to build";
    return std::nullopt;
  }
  if (candidate->status() == proto::Candidate::DISABLED) {
    *error = "candidate '" + candidate_id + "' is disabled";
    return std::nullopt;
  }

  const int wanted =
      games <= 0 ? config_.default_games
                 : std::min(games, config_.max_games_per_job);
  const auto opponents = ExpandOpponentsLocked(*candidate, opponent, error);
  if (!opponents.has_value()) {
    return std::nullopt;
  }
  return EnqueueLocked(*candidate, *opponents, wanted);
}

auto Scheduler::FreeSlotsLocked() const -> int {
  int free = 0;
  for (const auto &[id, state] : workers_) {
    free += state.worker->slots() - static_cast<int>(state.in_flight.size());
  }
  return free;
}

void Scheduler::DispatchLocked() {
  bool progress = true;
  while (progress) {
    progress = false;
    for (auto queued = queue_.begin(); queued != queue_.end();) {
      auto job_it = jobs_.find(*queued);
      if (job_it == jobs_.end() || job_it->second.pending.empty() ||
          job_it->second.aborted) {
        queued = queue_.erase(queued);
        continue;
      }
      Job &job = job_it->second;
      Batch &batch = job.pending.front();
      const int needed = static_cast<int>(batch.orders.size());
      if (FreeSlotsLocked() < needed) {
        ++queued;
        continue;
      }

      // Pick the emptiest workers first, so a two-order batch spreads across
      // hosts instead of piling both builds onto one.
      std::vector<WorkerState *> by_room;
      for (auto &[id, state] : workers_) {
        if (static_cast<int>(state.in_flight.size()) < state.worker->slots()) {
          by_room.push_back(&state);
        }
      }
      std::sort(by_room.begin(), by_room.end(),
                [](const WorkerState *a, const WorkerState *b) {
                  return a->worker->slots() - a->in_flight.size() >
                         b->worker->slots() - b->in_flight.size();
                });

      bool sent_all = true;
      std::vector<std::pair<WorkerState *, std::string>> sent;
      size_t next = 0;
      for (const proto::WorkOrder &order : batch.orders) {
        while (next < by_room.size() &&
               static_cast<int>(by_room[next]->in_flight.size()) >=
                   by_room[next]->worker->slots()) {
          ++next;
        }
        if (next >= by_room.size()) {
          sent_all = false;
          break;
        }
        WorkerState *state = by_room[next];
        proto::FleetMessage msg;
        *msg.mutable_order() = order;
        if (!state->worker->Send(msg)) {
          sent_all = false;
          break;
        }
        state->in_flight.push_back(order.order_id());
        order_owner_[order.order_id()] = {job.status.job_id(),
                                          state->worker->worker_id()};
        sent.emplace_back(state, order.order_id());
      }

      if (!sent_all) {
        // All-or-nothing: half a paired batch would park the built bot at the
        // broker's rendezvous with no partner ever arriving.
        for (const auto &[state, order_id] : sent) {
          std::erase(state->in_flight, order_id);
          order_owner_.erase(order_id);
        }
        ++queued;
        continue;
      }

      batch.dispatched = true;
      const std::string batch_key = batch.orders.front().order_id();
      job.running[batch_key] = std::move(batch);
      job.pending.pop_front();
      job.status.set_state(proto::Job::RUNNING);
      progress = true;

      if (job.pending.empty()) {
        queued = queue_.erase(queued);
      } else {
        ++queued;
      }
    }
  }
}

void Scheduler::AddWorker(std::shared_ptr<FleetWorker> worker) {
  std::lock_guard lock(mutex_);
  const std::string id = worker->worker_id();
  LOG(INFO) << "Sandbox worker '" << id << "' attached with "
            << worker->slots() << " slot(s)";
  workers_[id] = WorkerState{.worker = std::move(worker), .in_flight = {}};
  DispatchLocked();
}

void Scheduler::RemoveWorker(const std::string &worker_id) {
  std::lock_guard lock(mutex_);
  const auto it = workers_.find(worker_id);
  if (it == workers_.end()) {
    return;
  }
  const std::vector<std::string> orphaned = it->second.in_flight;
  workers_.erase(it);
  LOG(INFO) << "Sandbox worker '" << worker_id << "' detached with "
            << orphaned.size() << " order(s) in flight";

  // A dropped worker takes its half of a paired batch with it, so the whole
  // batch has to go back to the queue -- the surviving order's bot would
  // otherwise sit at the broker waiting for a partner that is never built.
  for (const std::string &order_id : orphaned) {
    const auto owner = order_owner_.find(order_id);
    if (owner == order_owner_.end()) {
      continue;
    }
    const std::string job_id = owner->second.first;
    order_owner_.erase(owner);
    auto job_it = jobs_.find(job_id);
    if (job_it == jobs_.end()) {
      continue;
    }
    Job &job = job_it->second;
    for (auto running = job.running.begin(); running != job.running.end();) {
      const auto &orders = running->second.orders;
      const bool mine =
          std::any_of(orders.begin(), orders.end(),
                      [&](const proto::WorkOrder &order) {
                        return order.order_id() == order_id;
                      });
      if (!mine) {
        ++running;
        continue;
      }
      Batch batch = std::move(running->second);
      running = job.running.erase(running);
      // Cancel the surviving side so its slot comes back now rather than after
      // its rendezvous times out.
      for (const proto::WorkOrder &order : batch.orders) {
        const auto other = order_owner_.find(order.order_id());
        if (other == order_owner_.end()) {
          continue;
        }
        const auto worker = workers_.find(other->second.second);
        if (worker != workers_.end()) {
          proto::FleetMessage msg;
          msg.mutable_cancel()->set_order_id(order.order_id());
          worker->second.worker->Send(msg);
          std::erase(worker->second.in_flight, order.order_id());
        }
        order_owner_.erase(other);
      }
      batch.dispatched = false;
      batch.outstanding = static_cast<int>(batch.orders.size());
      job.pending.push_front(std::move(batch));
      if (std::find(queue_.begin(), queue_.end(), job_id) == queue_.end()) {
        queue_.push_front(job_id);
      }
      break;
    }
  }
  DispatchLocked();
}

void Scheduler::OnResult(const std::string &worker_id,
                         const proto::OrderResult &result) {
  std::lock_guard lock(mutex_);
  const auto owner = order_owner_.find(result.order_id());
  if (owner == order_owner_.end()) {
    LOG(WARNING) << "Result for unknown order '" << result.order_id() << "'";
    return;
  }
  const std::string job_id = owner->second.first;
  order_owner_.erase(owner);

  const auto worker = workers_.find(worker_id);
  if (worker != workers_.end()) {
    std::erase(worker->second.in_flight, result.order_id());
  }

  auto job_it = jobs_.find(job_id);
  if (job_it == jobs_.end()) {
    DispatchLocked();
    return;
  }
  Job &job = job_it->second;

  // Which candidate actually ran this order: for a paired batch the failing
  // side may be the opponent, and it is that candidate whose build broke.
  std::string order_candidate = job.status.candidate_id();
  for (const auto &[key, batch] : job.running) {
    for (const proto::WorkOrder &order : batch.orders) {
      if (order.order_id() == result.order_id()) {
        order_candidate = order.candidate_id();
      }
    }
  }

  const bool primary = job.order_is_primary.contains(result.order_id()) &&
                       job.order_is_primary.at(result.order_id());
  if (primary) {
    job.status.set_games_played(job.status.games_played() +
                                result.games_played());
    job.status.set_wins(job.status.wins() + result.wins());
    job.status.set_draws(job.status.draws() + result.draws());
    job.status.set_losses(job.status.losses() + result.losses());
    if (result.elo() != 0.0) {
      job.status.set_elo(result.elo());
    }
  }

  if (!result.build_ok()) {
    // The candidate never compiled, so nothing downstream can run. Fail the
    // whole job rather than letting later batches retry a build that cannot
    // succeed.
    candidates_->SetStatus(order_candidate, proto::Candidate::BUILD_FAILED,
                           result.build_log());
    job.aborted = true;
    job.status.set_state(proto::Job::FAILED);
    job.status.set_error(
        result.build_log().empty()
            ? "build failed for " + order_candidate
            : order_candidate + ": " + result.build_log());
  } else if (!result.error().empty() && primary) {
    job.status.set_error(result.error());
  } else if (primary) {
    candidates_->SetStatus(job.status.candidate_id(), proto::Candidate::READY,
                           "");
  }

  // Retire the batch this order belonged to.
  for (auto running = job.running.begin(); running != job.running.end();) {
    const auto &orders = running->second.orders;
    const bool mine = std::any_of(orders.begin(), orders.end(),
                                  [&](const proto::WorkOrder &order) {
                                    return order.order_id() ==
                                           result.order_id();
                                  });
    if (!mine) {
      ++running;
      continue;
    }
    if (--running->second.outstanding <= 0) {
      job.running.erase(running);
    }
    break;
  }

  if (job.aborted) {
    job.pending.clear();
  }
  ConcludeJobLocked(&job);
  DispatchLocked();
}

void Scheduler::ConcludeJobLocked(Job *job) {
  if (!job->pending.empty() || !job->running.empty()) {
    return;
  }
  if (job->status.state() != proto::Job::FAILED) {
    job->status.set_state(proto::Job::DONE);
  }
  job->status.set_finished_unix_ms(NowUnixMs());
}

auto Scheduler::GetJob(const std::string &job_id) const
    -> std::optional<proto::Job> {
  std::lock_guard lock(mutex_);
  const auto it = jobs_.find(job_id);
  if (it == jobs_.end()) {
    return std::nullopt;
  }
  return it->second.status;
}

auto Scheduler::worker_count() const -> int {
  std::lock_guard lock(mutex_);
  return static_cast<int>(workers_.size());
}

auto Scheduler::queued_batches() const -> int {
  std::lock_guard lock(mutex_);
  int total = 0;
  for (const auto &[id, job] : jobs_) {
    total += static_cast<int>(job.pending.size());
  }
  return total;
}

auto Scheduler::in_flight_orders() const -> int {
  std::lock_guard lock(mutex_);
  return static_cast<int>(order_owner_.size());
}

}  // namespace tournament_arena
