#include "game_arena/server/scheduler.h"

#include <google/protobuf/repeated_ptr_field.h>

#include <algorithm>
#include <utility>

#include "absl/log/log.h"
#include "game_arena/server/problem_config.h"

namespace tournament_arena {

namespace {

int64_t NowUnixMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

constexpr std::string_view kBuiltinPrefix = "builtin:";
constexpr std::string_view kPlayerPrefix = "player:";

bool IsBuiltin(const std::string &opponent) {
  return opponent.rfind(kBuiltinPrefix, 0) == 0;
}

}  // namespace

Scheduler::Reservation::~Reservation() {
  if (scheduler_ == nullptr) {
    return;
  }
  // Never consumed -- the submission was rejected or failed to store -- so the
  // slot goes back rather than being held until restart.
  std::lock_guard lock(scheduler_->mutex_);
  scheduler_->ReleaseReservationLocked(client_id_);
}

Scheduler::Reservation::Reservation(Reservation &&other) noexcept
    : scheduler_(other.scheduler_),
      client_id_(std::move(other.client_id_)),
      superseded_(std::move(other.superseded_)) {
  other.scheduler_ = nullptr;
}

auto Scheduler::Reservation::operator=(Reservation &&other) noexcept
    -> Reservation & {
  if (this != &other) {
    scheduler_ = other.scheduler_;
    client_id_ = std::move(other.client_id_);
    superseded_ = std::move(other.superseded_);
    other.scheduler_ = nullptr;
  }
  return *this;
}

Scheduler::Scheduler(SchedulerConfig config, CandidateStore *candidates,
                     Standings *standings)
    : config_(std::move(config)),
      candidates_(candidates),
      standings_(standings) {}

void Scheduler::ReleaseReservationLocked(const std::string &client_id) {
  const auto it = reserved_.find(client_id);
  if (it != reserved_.end() && --it->second <= 0) {
    reserved_.erase(it);
  }
}

void Scheduler::AbortJobLocked(Job *job, const std::string &reason) {
  job->aborted = true;
  job->pending.clear();
  for (const auto &[order_id, order] : job->running) {
    const auto owner = order_owner_.find(order_id);
    if (owner == order_owner_.end()) {
      continue;
    }
    const auto worker = workers_.find(owner->second.second);
    if (worker != workers_.end()) {
      proto::FleetMessage msg;
      msg.mutable_cancel()->set_order_id(order_id);
      worker->second.worker->Send(msg);
      std::erase(worker->second.in_flight, order_id);
    }
    order_owner_.erase(owner);
  }
  job->running.clear();
  // CANCELLED rather than FAILED: an agent polling this needs to tell "you
  // replaced it" from "it broke".
  job->status.set_state(proto::Job::CANCELLED);
  job->status.set_error(reason);
  job->status.set_finished_unix_ms(NowUnixMs());
}

auto Scheduler::TryReserve(const std::string &client_id,
                           const proto::ClientQuota &quota, bool cancel_running,
                           std::string *error) -> std::optional<Reservation> {
  Reservation reservation;
  reservation.client_id_ = client_id;
  if (client_id.empty()) {
    // No registry configured: nobody to meter. The reservation is inert, and
    // its destructor has nothing to release.
    return reservation;
  }

  std::lock_guard lock(mutex_);
  int running = 0;
  int queued = reserved_[client_id];
  std::vector<std::string> replaceable;
  for (auto &[job_id, job] : jobs_) {
    if (job.client_id != client_id) {
      continue;
    }
    if (job.status.state() == proto::Job::RUNNING) {
      ++running;
      replaceable.push_back(job_id);
    } else if (job.status.state() == proto::Job::QUEUED) {
      ++queued;
      replaceable.push_back(job_id);
    }
  }

  const int max_active =
      std::max(1, static_cast<int>(quota.max_active_evaluations()));
  const int max_queued = std::max(1, static_cast<int>(quota.max_queued_jobs()));

  // Unconditional, not only when over quota: a client that asks to replace its
  // work means it, and "sometimes replaces, depending on a limit you cannot
  // see" is the harder behaviour to reason about.
  if (cancel_running && !replaceable.empty()) {
    for (const std::string &job_id : replaceable) {
      auto it = jobs_.find(job_id);
      if (it != jobs_.end()) {
        AbortJobLocked(&it->second,
                       "superseded by a newer submission from " + client_id);
        reservation.superseded_.push_back(job_id);
      }
    }
    running = 0;
    queued = reserved_[client_id];
  }

  if (running >= max_active) {
    *error = "you already have " + std::to_string(running) +
             " evaluation(s) running (limit " + std::to_string(max_active) +
             "). Poll arena_job until it finishes, or resubmit with "
             "cancel_running to replace it. Running: " +
             (replaceable.empty() ? std::string("-") : replaceable.front());
    return std::nullopt;
  }
  if (queued >= max_queued) {
    *error = "you already have " + std::to_string(queued) +
             " job(s) queued (limit " + std::to_string(max_queued) + ")";
    return std::nullopt;
  }

  ++reserved_[client_id];
  reservation.scheduler_ = this;
  return reservation;
}

bool Scheduler::FillSideLocked(const proto::Candidate &candidate,
                               proto::Side *side) const {
  side->set_candidate_id(candidate.candidate_id());
  *side->mutable_params() = candidate.params();

  // The patch is the submission. It travels with the order so a worker needs
  // nothing but the repo and this message -- no callback to the arena, no
  // shared filesystem.
  // From the candidate as handed over, not looked up by id: a resubmit is
  // staged beside the entry it replaces, and the two share one.
  if (candidate.patch().empty()) {
    LOG(ERROR) << "Candidate " << candidate.candidate_id() << " has no patch";
    return false;
  }
  side->set_patch(candidate.patch());

  // "{submission_id}" is expanded here, so the worker never sees a template and
  // needs no problem config of its own.
  for (const std::string &target : config_.build_targets()) {
    side->add_build_targets(
        ExpandSubmissionId(target, candidate.candidate_id()));
  }
  side->set_bot_target(
      ExpandSubmissionId(config_.bot_target(), candidate.candidate_id()));
  return true;
}

std::optional<proto::WorkOrder> Scheduler::MakeOrderLocked(
    const proto::Candidate &candidate, const std::string &opponent, int games,
    const std::string &job_id) {
  proto::WorkOrder order;
  order.set_order_id("o" + std::to_string(NowUnixMs()) + "_" +
                     std::to_string(++order_counter_));
  order.set_job_id(job_id);
  order.set_game(candidate.game());
  order.set_opponent_spec(opponent);
  order.set_num_games(games);
  order.set_build_timeout_s(config_.build_timeout_s());
  order.set_run_timeout_s(config_.run_timeout_s());
  order.set_referee_target(config_.referee_target());
  order.set_match_deadline_s(config_.match_deadline_s());
  *order.mutable_sandbox() = config_.sandbox();
  *order.mutable_bazel_flags() = config_.bazel_flags();
  order.set_turn_timeout_ms(config_.turn_timeout_ms());
  order.set_game_time_budget_ms(config_.game_time_budget_ms());
  order.set_max_moves_per_game(config_.max_moves_per_game());
  *order.mutable_registry_options() = config_.registry_options();

  if (!FillSideLocked(candidate, order.mutable_candidate())) {
    return std::nullopt;
  }

  if (config_.has_grade()) {
    // A graded order has no opponent: running the command *is* the whole
    // evaluation.
    *order.mutable_grade() = config_.grade();
    order.mutable_grade()->clear_argv();
    for (const std::string &arg : config_.grade().argv()) {
      order.mutable_grade()->add_argv(
          ExpandSubmissionId(arg, candidate.candidate_id()));
    }
    order.clear_referee_target();
    return order;
  }

  if (IsBuiltin(opponent)) {
    return order;
  }

  // A candidate opponent rides along in the same order. That is what makes an
  // order a whole match: the worker builds both sides and referees them
  // itself, so there is no second order to keep in step with this one.
  const std::string rival_id = opponent.rfind(kPlayerPrefix, 0) == 0
                                   ? opponent.substr(kPlayerPrefix.size())
                                   : opponent;
  const auto rival = candidates_->Get(rival_id);
  if (!rival.has_value() || rival->status() != proto::Candidate::READY) {
    return std::nullopt;
  }
  order.set_opponent_spec(std::string(kPlayerPrefix) + rival->candidate_id());
  if (!FillSideLocked(*rival, order.mutable_opponent())) {
    return std::nullopt;
  }
  return order;
}

std::string Scheduler::EnqueueLocked(const proto::Candidate &candidate,
                                     const std::vector<std::string> &opponents,
                                     int games, const std::string &client_id) {
  const std::string job_id =
      "j" + std::to_string(NowUnixMs()) + "_" + std::to_string(++job_counter_);

  Job job;
  job.client_id = client_id;
  job.status.set_job_id(job_id);
  job.status.set_candidate_id(candidate.candidate_id());
  job.status.set_state(proto::Job::QUEUED);
  job.status.set_created_unix_ms(NowUnixMs());

  int requested = 0;
  for (const std::string &opponent : opponents) {
    auto order = MakeOrderLocked(candidate, opponent, games, job_id);
    if (!order.has_value()) {
      LOG(WARNING) << "Job " << job_id << ": skipping opponent '" << opponent
                   << "' (not runnable)";
      continue;
    }
    requested += games;
    job.pending.push_back(std::move(*order));
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

std::string Scheduler::EnqueuePlacement(const proto::Candidate &candidate,
                                        Reservation reservation) {
  std::lock_guard lock(mutex_);
  // Consumed: the slot it held becomes the job below, so the destructor must
  // not hand it back.
  const std::string client_id = reservation.client_id();
  if (reservation.scheduler_ != nullptr) {
    ReleaseReservationLocked(client_id);
    reservation.scheduler_ = nullptr;
  }
  // A participant's older work is for code this submission replaces, and its
  // result would be taken for this one's.
  for (auto &[job_id, job] : jobs_) {
    if (job.status.candidate_id() == candidate.candidate_id() &&
        (job.status.state() == proto::Job::QUEUED ||
         job.status.state() == proto::Job::RUNNING)) {
      AbortJobLocked(&job, "superseded by a newer submission");
    }
  }
  if (config_.has_grade()) {
    // A graded problem has no opponents to be placed against: the one order is
    // the whole measurement. The empty entry is that order.
    return EnqueueLocked(candidate, {""}, config_.placement_games(), client_id);
  }
  std::vector<std::string> opponents(config_.placement_opponents().begin(),
                                     config_.placement_opponents().end());
  const std::vector<std::string> ladder =
      LadderLocked(candidate.candidate_id());
  opponents.insert(opponents.end(), ladder.begin(), ladder.end());
  return EnqueueLocked(candidate, opponents, config_.placement_games(),
                       client_id);
}

// Rated rivals, evenly spaced from the top of the board to the bottom.
std::vector<std::string> Scheduler::LadderLocked(
    const std::string &self) const {
  std::vector<std::string> rated;
  if (standings_ != nullptr) {
    for (const Standing &row : standings_->Rank(0)) {
      if (row.candidate_id != self) {
        rated.push_back(row.candidate_id);
      }
    }
  }
  const std::size_t n = rated.size();
  const std::size_t k =
      std::min(n, static_cast<std::size_t>(std::max(0, config_.ladder_size())));
  std::vector<std::string> ladder;
  for (std::size_t i = 0; i < k; ++i) {
    ladder.push_back(rated[k == 1 ? 0 : i * (n - 1) / (k - 1)]);
  }
  return ladder;
}

int Scheduler::FreeSlotsLocked() const {
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

      // The emptiest worker first, so work spreads across hosts instead of
      // filling one before touching the next.
      WorkerState *best = nullptr;
      for (auto &[id, state] : workers_) {
        const int room =
            state.worker->slots() - static_cast<int>(state.in_flight.size());
        if (room <= 0) {
          continue;
        }
        if (best == nullptr ||
            room > best->worker->slots() -
                       static_cast<int>(best->in_flight.size())) {
          best = &state;
        }
      }
      if (best == nullptr) {
        ++queued;
        continue;
      }

      proto::WorkOrder order = std::move(job.pending.front());
      proto::FleetMessage msg;
      *msg.mutable_order() = order;
      if (!best->worker->Send(msg)) {
        // The worker is gone; RemoveWorker will clean it up when its stream
        // ends. Leave the order queued rather than losing it.
        ++queued;
        continue;
      }
      job.pending.pop_front();
      best->in_flight.push_back(order.order_id());
      order_owner_[order.order_id()] = {job.status.job_id(),
                                        best->worker->worker_id()};
      job.running[order.order_id()] = std::move(order);
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
  LOG(INFO) << "Sandbox worker '" << id << "' attached with " << worker->slots()
            << " slot(s)";
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

  // A dropped worker's orders go back to the queue. Each is self-contained, so
  // requeueing one is just requeueing one -- there is no sibling order on
  // another host to cancel, and no half-match to unwind.
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
    const auto running = job.running.find(order_id);
    if (running == job.running.end()) {
      continue;
    }
    job.pending.push_front(std::move(running->second));
    job.running.erase(running);
    if (std::find(queue_.begin(), queue_.end(), job_id) == queue_.end()) {
      queue_.push_front(job_id);
    }
  }
  DispatchLocked();
}

void Scheduler::OnProgress(const proto::OrderProgress &progress) {
  std::lock_guard lock(mutex_);
  const auto owner = order_owner_.find(progress.order_id());
  if (owner == order_owner_.end()) {
    // The order has already been retired by its result, or never existed.
    // Either way there is nothing to annotate and nothing to complain about.
    return;
  }
  const auto job_it = jobs_.find(owner->second.first);
  if (job_it == jobs_.end()) {
    return;
  }
  job_it->second.status.set_phase(progress.phase());
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

  // Every order is now the job's own whole evaluation, so its tally is the
  // job's tally -- no "is this half of a pair mine" question to answer.
  job.status.set_games_played(job.status.games_played() +
                              result.games_played());
  job.status.set_wins(job.status.wins() + result.wins());
  job.status.set_draws(job.status.draws() + result.draws());
  job.status.set_losses(job.status.losses() + result.losses());

  if (!result.build_ok()) {
    // An order builds both sides. Whose build broke decides who gets retired,
    // and the worker is the only one who knows -- so it says. Falling back to
    // the job's own candidate keeps an older worker's result usable.
    const std::string &broken = result.build_failed_candidate_id().empty()
                                    ? job.status.candidate_id()
                                    : result.build_failed_candidate_id();
    candidates_->SetStatus(broken, proto::Candidate::BUILD_FAILED,
                           result.build_log());
    job.aborted = true;
    job.status.set_state(proto::Job::FAILED);
    if (broken == job.status.candidate_id()) {
      job.status.set_error(result.build_log().empty()
                               ? "build failed for " + broken
                               : broken + ": " + result.build_log());
    } else {
      // Worth distinguishing: the submitter did nothing wrong, and telling
      // them "your build failed" would send them hunting their own code.
      job.status.set_error("opponent " + broken +
                           " failed to build; this candidate was not at fault");
    }
  } else if (!result.error().empty()) {
    job.status.set_error(result.error());
  } else {
    candidates_->SetStatus(job.status.candidate_id(), proto::Candidate::READY,
                           "");
    // The coordinator owns the standings. A match's referee kept its own
    // ratings while it played, but those died with its container.
    if (standings_ != nullptr) {
      const auto running = job.running.find(result.order_id());
      const std::string opponent = running != job.running.end()
                                       ? running->second.opponent_spec()
                                       : std::string();
      standings_->Record(job.status.candidate_id(), opponent, result);
      job.status.set_elo(standings_->Get(job.status.candidate_id()).score);
    }
  }

  job.running.erase(result.order_id());

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

std::optional<proto::Job> Scheduler::GetJob(const std::string &job_id) const {
  std::lock_guard lock(mutex_);
  const auto it = jobs_.find(job_id);
  if (it == jobs_.end()) {
    return std::nullopt;
  }
  return it->second.status;
}

int Scheduler::worker_count() const {
  std::lock_guard lock(mutex_);
  return static_cast<int>(workers_.size());
}

int Scheduler::queued_orders() const {
  std::lock_guard lock(mutex_);
  int total = 0;
  for (const auto &[id, job] : jobs_) {
    total += static_cast<int>(job.pending.size());
  }
  return total;
}

int Scheduler::in_flight_orders() const {
  std::lock_guard lock(mutex_);
  return static_cast<int>(order_owner_.size());
}

}  // namespace tournament_arena
