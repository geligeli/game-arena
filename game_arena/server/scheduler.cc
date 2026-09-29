#include "game_arena/server/scheduler.h"

#include <google/protobuf/repeated_ptr_field.h>

#include <algorithm>
#include <utility>

#include "absl/log/log.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_join.h"
#include "absl/time/clock.h"
#include "absl/time/time.h"
#include "game_arena/common/sha256/sha256.h"
#include "game_arena/server/problem_config.h"

namespace tournament_arena {

namespace {

constexpr std::string_view kBuiltinPrefix = "builtin:";
constexpr std::string_view kPlayerPrefix = "player:";

bool IsBuiltin(const std::string &opponent) {
  return opponent.rfind(kBuiltinPrefix, 0) == 0;
}

JobRecord::Order *OrderIn(JobRecord *record, const std::string &order_id) {
  for (JobRecord::Order &order : *record->mutable_orders()) {
    if (order.order_id() == order_id) {
      return &order;
    }
  }
  return nullptr;
}

}  // namespace

void Scheduler::Reservation::Release::operator()(Scheduler *scheduler) const {
  std::lock_guard lock(scheduler->mutex_);
  scheduler->ReleaseReservationLocked(client_id);
}

Scheduler::Scheduler(SchedulerConfig config, CandidateStore *candidates,
                     Standings *standings,
                     tournament_broker::GameHistory *history, JobLog *job_log,
                     std::function<void(const proto::Job &)> on_concluded,
                     ArtifactStore *artifacts)
    : config_(std::move(config)),
      candidates_(candidates),
      standings_(standings),
      history_(history),
      job_log_(job_log),
      on_concluded_(std::move(on_concluded)),
      artifacts_(artifacts),
      referee_ref_("referee-" +
                   sha256::Hex(absl::StrCat(
                       config_.order().sandbox().image(), "\n",
                       absl::StrJoin(config_.order().bazel_flags(), " "), "\n",
                       config_.order().referee_target()))) {}

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
  // CANCELLED, not FAILED: "you replaced it" is not "it broke".
  job->status.set_state(proto::Job::CANCELLED);
  job->status.set_error(reason);
  job->status.set_finished_unix_ms(absl::ToUnixMillis(absl::Now()));
  PersistLocked(job);
}

auto Scheduler::TryReserve(const std::string &client_id,
                           const proto::ClientQuota &quota, bool cancel_running,
                           std::string *error) -> std::optional<Reservation> {
  Reservation reservation;
  reservation.held_.get_deleter().client_id = client_id;
  if (client_id.empty()) {
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

  // Unconditional, not only when over quota: no limit the client cannot see.
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
  reservation.held_.reset(this);
  return reservation;
}

void Scheduler::FillSideLocked(const proto::Candidate &candidate,
                               proto::Side *side) const {
  side->set_candidate_id(candidate.candidate_id());
  // The referee plays it: nothing to patch or build.
  if (IsBuiltin(candidate.candidate_id())) {
    return;
  }
  *side->mutable_params() = candidate.params();

  // Not looked up by id: a staged resubmit shares its entry's id.
  side->set_patch(candidate.patch());
  side->set_artifact(candidate.artifact());

  // Expanded here, so the worker needs no problem config of its own.
  for (const std::string &target : config_.build_targets()) {
    side->add_build_targets(
        ExpandSubmissionId(target, candidate.candidate_id()));
  }
  side->set_bot_target(
      ExpandSubmissionId(config_.bot_target(), candidate.candidate_id()));
}

std::optional<proto::WorkOrder> Scheduler::MakeOrderLocked(
    const proto::Candidate &candidate, const std::string &opponent, int games,
    const std::string &job_id) {
  proto::WorkOrder order = config_.order();
  order.set_order_id(NextOrderIdLocked());
  order.set_job_id(job_id);
  order.set_game(candidate.game());
  order.set_opponent_spec(opponent);
  order.set_num_games(games);
  if (artifacts_ != nullptr) {
    order.set_referee_artifact(artifacts_->Ref(referee_ref_));
  }

  FillSideLocked(candidate, order.mutable_candidate());

  if (order.has_grade()) {
    for (std::string &arg : *order.mutable_grade()->mutable_argv()) {
      arg = ExpandSubmissionId(arg, candidate.candidate_id());
    }
    return order;
  }

  if (IsBuiltin(opponent)) {
    return order;
  }

  // The rival's side rides in the same order: one order is one whole match.
  const std::string rival_id = opponent.rfind(kPlayerPrefix, 0) == 0
                                   ? opponent.substr(kPlayerPrefix.size())
                                   : opponent;
  const auto rival = candidates_->Get(rival_id);
  if (!rival.has_value() || rival->status() != proto::Candidate::READY) {
    return std::nullopt;
  }
  order.set_opponent_spec(std::string(kPlayerPrefix) + rival->candidate_id());
  FillSideLocked(*rival, order.mutable_opponent());
  return order;
}

std::string Scheduler::NextOrderIdLocked() {
  return "o" + std::to_string(absl::ToUnixMillis(absl::Now())) + "_" +
         std::to_string(++order_counter_);
}

proto::WorkOrder Scheduler::MakeBuildOrderLocked(
    const proto::Candidate &candidate, const std::string &job_id) {
  proto::WorkOrder order = config_.order();
  order.set_order_id(NextOrderIdLocked());
  order.set_job_id(job_id);
  order.set_game(candidate.game());
  order.set_build_only(true);
  FillSideLocked(candidate, order.mutable_candidate());
  order.mutable_candidate()->clear_artifact();
  // The referee, too, until the archive has one for this image.
  if (!artifacts_->Ref(referee_ref_).empty()) {
    order.clear_referee_target();
  }
  return order;
}

std::string Scheduler::EnqueueLocked(const proto::Candidate &candidate,
                                     const std::vector<std::string> &opponents,
                                     int games, const std::string &client_id,
                                     bool record_patch, bool build_first) {
  const std::string job_id = "j" +
                             std::to_string(absl::ToUnixMillis(absl::Now())) +
                             "_" + std::to_string(++job_counter_);

  Job job;
  job.client_id = client_id;
  job.status.set_job_id(job_id);
  job.status.set_candidate_id(candidate.candidate_id());
  job.status.set_state(proto::Job::QUEUED);
  job.status.set_created_unix_ms(absl::ToUnixMillis(absl::Now()));
  *job.record.mutable_submission() = candidate;
  if (!record_patch) {
    job.record.mutable_submission()->clear_patch();
  }

  int requested = 0;
  if (build_first) {
    job.after_build = opponents;
    job.after_build_games = games;
    requested = games * static_cast<int>(opponents.size());
    proto::WorkOrder order = MakeBuildOrderLocked(candidate, job_id);
    JobRecord::Order *logged = job.record.add_orders();
    logged->set_order_id(order.order_id());
    logged->set_opponent_spec("build");
    job.pending.push_back(std::move(order));
  }
  for (const std::string &opponent :
       build_first ? std::vector<std::string>{} : opponents) {
    auto order = MakeOrderLocked(candidate, opponent, games, job_id);
    if (!order.has_value()) {
      LOG(WARNING) << "Job " << job_id << ": skipping opponent '" << opponent
                   << "' (not runnable)";
      continue;
    }
    requested += games;
    JobRecord::Order *logged = job.record.add_orders();
    logged->set_order_id(order->order_id());
    logged->set_opponent_spec(order->opponent_spec());
    job.pending.push_back(std::move(*order));
  }
  job.status.set_games_requested(requested);

  jobs_[job_id] = std::move(job);
  if (jobs_[job_id].pending.empty()) {
    // Nothing runnable: fail now rather than leave the caller polling.
    jobs_[job_id].status.set_state(proto::Job::FAILED);
    jobs_[job_id].status.set_error("no runnable opponent");
    ConcludeJobLocked(&jobs_[job_id]);
    PersistLocked(&jobs_[job_id]);
    return job_id;
  }
  queue_.push_back(job_id);
  PersistLocked(&jobs_[job_id]);
  DispatchLocked();
  return job_id;
}

std::string Scheduler::EnqueuePlacement(const proto::Candidate &candidate,
                                        Reservation reservation) {
  std::lock_guard lock(mutex_);
  // Consumed: the slot becomes the job below.
  const std::string client_id = reservation.client_id();
  if (reservation.held_.release() != nullptr) {
    ReleaseReservationLocked(client_id);
  }
  // Older work is for code this replaces; its result would pass for this one's.
  for (auto &[job_id, job] : jobs_) {
    if (job.status.candidate_id() == candidate.candidate_id() &&
        (job.status.state() == proto::Job::QUEUED ||
         job.status.state() == proto::Job::RUNNING)) {
      AbortJobLocked(&job, "superseded by a newer submission");
    }
  }
  if (config_.order().has_grade()) {
    // A graded problem's one order has no opponent: the empty entry.
    return EnqueueLocked(candidate, {""}, config_.placement_games(), client_id);
  }
  std::vector<std::string> opponents(config_.placement_opponents().begin(),
                                     config_.placement_opponents().end());
  const std::vector<std::string> ladder = LadderLocked(candidate);
  opponents.insert(opponents.end(), ladder.begin(), ladder.end());
  return EnqueueLocked(
      candidate, opponents, config_.placement_games(), client_id,
      /*record_patch=*/true,
      /*build_first=*/artifacts_ != nullptr && candidate.artifact().empty());
}

std::string Scheduler::EnqueueBuild(const proto::Candidate &candidate) {
  std::lock_guard lock(mutex_);
  const std::string job_id = EnqueueLocked(candidate, {}, 0, "",
                                           /*record_patch=*/false,
                                           /*build_first=*/true);
  jobs_[job_id].backfill = true;
  return job_id;
}

std::string Scheduler::EnqueueMatch(const proto::Candidate &candidate,
                                    const std::string &opponent, int games) {
  std::lock_guard lock(mutex_);
  return EnqueueLocked(candidate, {opponent}, games, "",
                       /*record_patch=*/false);
}

// Rated rivals, evenly spaced from the top of the board to the bottom.
std::vector<std::string> Scheduler::LadderLocked(
    const proto::Candidate &candidate) const {
  // Not itself, nor, when every submission is a version, its author's other
  // versions: placement measures a newcomer against other participants.
  const auto same_author = [&](const std::string &id) {
    if (id == candidate.candidate_id()) {
      return true;
    }
    const auto rival = candidates_->Get(id);
    return !candidate.author().empty() && rival.has_value() &&
           rival->author() == candidate.author();
  };
  std::vector<std::string> rated;
  if (standings_ != nullptr) {
    for (const Standing &row : standings_->Rank(0)) {
      if (!same_author(row.candidate_id)) {
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

      // The emptiest worker first, so work spreads across hosts; a build
      // order only to one that uploads what it built.
      const bool build_only = job.pending.front().build_only();
      const auto room = [build_only](const auto &entry) {
        return build_only && !entry.second.worker->builds_artifacts()
                   ? 0
                   : entry.second.worker->slots() -
                         static_cast<int>(entry.second.in_flight.size());
      };
      const auto emptiest = std::ranges::max_element(workers_, {}, room);
      if (emptiest == workers_.end() || room(*emptiest) <= 0) {
        ++queued;
        continue;
      }
      WorkerState *best = &emptiest->second;

      proto::WorkOrder order = std::move(job.pending.front());
      proto::FleetMessage msg;
      *msg.mutable_order() = order;
      if (!best->worker->Send(msg)) {
        // Gone: RemoveWorker cleans up when its stream ends. Stays queued.
        ++queued;
        continue;
      }
      job.pending.pop_front();
      best->in_flight.push_back(order.order_id());
      order_owner_[order.order_id()] = {job.status.job_id(),
                                        best->worker->worker_id()};
      job.running[order.order_id()] = std::move(order);
      if (job.status.state() != proto::Job::RUNNING) {
        job.status.set_state(proto::Job::RUNNING);
        PersistLocked(&job);
      }
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

  // Each order is self-contained, so requeueing one unwinds nothing else.
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
    return;
  }
  const auto job_it = jobs_.find(owner->second.first);
  if (job_it == jobs_.end()) {
    return;
  }
  job_it->second.status.set_phase(progress.phase());
}

void Scheduler::OnGame(const proto::OrderGame &game) {
  tournament_broker::proto::GameRecord record;
  if (history_ == nullptr || !record.ParseFromString(game.record())) {
    return;
  }
  // A referee numbers its games per process, so two of them can pick one id.
  record.set_game_id(game.order_id() + "-" + record.game_id());
  {
    std::lock_guard lock(mutex_);
    const auto owner = order_owner_.find(game.order_id());
    if (owner == order_owner_.end()) {
      return;
    }
    const auto job = jobs_.find(owner->second.first);
    if (job != jobs_.end()) {
      if (JobRecord::Order *order =
              OrderIn(&job->second.record, game.order_id())) {
        order->add_game_ids(record.game_id());
      }
    }
  }
  history_->Store(record);
  standings_->RecordGame(record);
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

  job.status.set_games_played(job.status.games_played() +
                              result.games_played());
  job.status.set_wins(job.status.wins() + result.wins());
  job.status.set_draws(job.status.draws() + result.draws());
  job.status.set_losses(job.status.losses() + result.losses());

  // A job the engine could not run carries an error and blames no one: it
  // says nothing about anyone's code.
  const auto running = job.running.find(result.order_id());
  if (running != job.running.end() && running->second.build_only()) {
    OnBuiltLocked(&job, running->second, result);
  } else if (!result.build_ok() &&
             (!result.build_failed_candidate_id().empty() ||
              result.error().empty())) {
    // Only the worker knows whose build broke; an older one does not say.
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
      // Not "your build failed": that would send them hunting their own code.
      job.status.set_error("opponent " + broken +
                           " failed to build; this candidate was not at fault");
    }
  } else if (!result.error().empty()) {
    job.status.set_error(result.error());
  } else {
    candidates_->SetStatus(job.status.candidate_id(), proto::Candidate::READY,
                           "");
    // Only the coordinator writes standings; a referee's die with it.
    if (standings_ != nullptr) {
      const std::string opponent = running != job.running.end()
                                       ? running->second.opponent_spec()
                                       : std::string();
      standings_->Record(job.status.candidate_id(), opponent, result);
      job.status.set_elo(standings_->Get(job.status.candidate_id()).score);
    }
  }

  if (JobRecord::Order *order = OrderIn(&job.record, result.order_id())) {
    *order->mutable_result() = result;
  }
  job.running.erase(result.order_id());

  if (job.aborted) {
    job.pending.clear();
  }
  ConcludeJobLocked(&job);
  PersistLocked(&job);
  DispatchLocked();
}

void Scheduler::OnBuiltLocked(Job *job, const proto::WorkOrder &order,
                              const proto::OrderResult &result) {
  const std::string &id = job->status.candidate_id();
  const std::string &bot = order.candidate().bot_target();
  const auto artifact = result.artifacts().find(bot);
  if (!result.build_ok() && !result.build_failed_candidate_id().empty()) {
    job->aborted = true;
    job->status.set_state(proto::Job::FAILED);
    job->status.set_error(id + ": " + result.build_log());
    // A backfill's candidate built before, and keeps playing the old way.
    if (!job->backfill) {
      candidates_->SetStatus(id, proto::Candidate::BUILD_FAILED,
                             result.build_log());
    }
    return;
  }
  if (!result.build_ok() || artifact == result.artifacts().end() ||
      !artifacts_->Has(artifact->second)) {
    // The engine's failure, not the code's: again, a few times.
    if (++job->build_attempts < 3 && !job->aborted) {
      proto::WorkOrder again = order;
      again.set_order_id(NextOrderIdLocked());
      JobRecord::Order *logged = job->record.add_orders();
      logged->set_order_id(again.order_id());
      logged->set_opponent_spec("build");
      job->pending.push_back(std::move(again));
      queue_.push_back(job->status.job_id());
      return;
    }
    job->status.set_state(proto::Job::FAILED);
    job->status.set_error(result.error().empty()
                              ? "the build uploaded no " + bot
                              : result.error());
    return;
  }

  if (const auto referee = result.artifacts().find(order.referee_target());
      !order.referee_target().empty() && referee != result.artifacts().end()) {
    artifacts_->SetRef(referee_ref_, referee->second);
  }
  candidates_->SetStatus(id, proto::Candidate::READY, "", artifact->second);
  const auto built = candidates_->Get(id);
  for (const std::string &opponent : job->after_build) {
    auto match = MakeOrderLocked(*built, opponent, job->after_build_games,
                                 job->status.job_id());
    if (!match.has_value()) {
      continue;
    }
    JobRecord::Order *logged = job->record.add_orders();
    logged->set_order_id(match->order_id());
    logged->set_opponent_spec(match->opponent_spec());
    job->pending.push_back(std::move(*match));
  }
  job->after_build.clear();
  if (!job->pending.empty()) {
    queue_.push_back(job->status.job_id());
  }
}

void Scheduler::ConcludeJobLocked(Job *job) {
  if (!job->pending.empty() || !job->running.empty()) {
    return;
  }
  if (job->status.state() != proto::Job::FAILED) {
    job->status.set_state(proto::Job::DONE);
  }
  job->status.set_finished_unix_ms(absl::ToUnixMillis(absl::Now()));
  if (on_concluded_) {
    on_concluded_(job->status);
  }
}

void Scheduler::PersistLocked(Job *job) {
  if (job_log_ == nullptr) {
    return;
  }
  *job->record.mutable_job() = job->status;
  job_log_->Put(job->record);
  // jobs_ keeps every job, and a record carries patches and build output.
  if (job->pending.empty() && job->running.empty()) {
    job->record.Clear();
  }
}

std::optional<proto::Job> Scheduler::GetJob(const std::string &job_id) const {
  std::lock_guard lock(mutex_);
  const auto it = jobs_.find(job_id);
  if (it != jobs_.end()) {
    return it->second.status;
  }
  // From before a restart: the log has how it ended.
  if (job_log_ != nullptr) {
    if (const auto record = job_log_->Get(job_id)) {
      return record->job();
    }
  }
  return std::nullopt;
}

int Scheduler::worker_count() const {
  std::lock_guard lock(mutex_);
  return static_cast<int>(workers_.size());
}

int Scheduler::total_slots() const {
  std::lock_guard lock(mutex_);
  int slots = 0;
  for (const auto &[id, state] : workers_) {
    slots += state.worker->slots();
  }
  return slots;
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
