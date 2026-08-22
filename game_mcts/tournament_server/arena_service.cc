#include "cpp/tournament_server/arena_service.h"

#include <algorithm>
#include <utility>
#include <vector>

#include "absl/log/log.h"

namespace tournament_arena {

ArenaService::ArenaService(CandidateStore *candidates, Scheduler *scheduler,
                           tournament_broker::EloStore *elo_store,
                           std::string base_commit, int default_list_limit)
    : candidates_(candidates),
      scheduler_(scheduler),
      elo_store_(elo_store),
      base_commit_(std::move(base_commit)),
      default_list_limit_(default_list_limit) {}

auto ArenaService::StandingFor(const proto::Candidate &candidate) const
    -> proto::CandidateStanding {
  proto::CandidateStanding standing;
  *standing.mutable_candidate() = candidate;
  // The candidate id is the broker player name, so its rating is already
  // there -- the arena keeps no scoreboard of its own.
  const auto rating =
      elo_store_->Get(candidate.game(), candidate.candidate_id());
  standing.set_elo(rating.elo());
  standing.set_wins(rating.wins());
  standing.set_draws(rating.draws());
  standing.set_losses(rating.losses());
  return standing;
}

auto ArenaService::Submit(grpc::ServerContext * /*context*/,
                          const proto::SubmitRequest *request,
                          proto::SubmitResponse *response) -> grpc::Status {
  std::string error;
  const auto candidate = candidates_->Create(*request, base_commit_, &error);
  if (!candidate.has_value()) {
    // Rejections are the agent's to fix, so the message is the whole payload.
    return {grpc::StatusCode::INVALID_ARGUMENT, error};
  }
  response->set_candidate_id(candidate->candidate_id());
  response->set_job_id(scheduler_->EnqueuePlacement(*candidate));
  return grpc::Status::OK;
}

auto ArenaService::GetCandidate(grpc::ServerContext * /*context*/,
                                const proto::GetCandidateRequest *request,
                                proto::Candidate *response) -> grpc::Status {
  const auto candidate = candidates_->Get(request->candidate_id());
  if (!candidate.has_value()) {
    return {grpc::StatusCode::NOT_FOUND,
            "unknown candidate '" + request->candidate_id() + "'"};
  }
  *response = *candidate;
  return grpc::Status::OK;
}

auto ArenaService::GetSource(grpc::ServerContext * /*context*/,
                             const proto::GetSourceRequest *request,
                             proto::SourceFile *response) -> grpc::Status {
  std::string error;
  const auto content = candidates_->ReadSource(request->candidate_id(),
                                               request->path(), &error);
  if (!content.has_value()) {
    return {grpc::StatusCode::NOT_FOUND, error};
  }
  response->set_path(request->path());
  response->set_content(*content);
  return grpc::Status::OK;
}

auto ArenaService::ListCandidates(grpc::ServerContext * /*context*/,
                                  const proto::ListCandidatesRequest *request,
                                  proto::ListCandidatesResponse *response)
    -> grpc::Status {
  std::vector<proto::CandidateStanding> rows;
  for (const proto::Candidate &candidate : candidates_->List()) {
    if (!request->game().empty() && candidate.game() != request->game()) {
      continue;
    }
    if (!request->author().empty() && candidate.author() != request->author()) {
      continue;
    }
    rows.push_back(StandingFor(candidate));
  }

  if (request->order() == proto::ListCandidatesRequest::ELO_DESC) {
    std::stable_sort(rows.begin(), rows.end(),
                     [](const auto &a, const auto &b) {
                       return a.elo() > b.elo();
                     });
  }  // NEWEST: CandidateStore::List already returns newest first.

  const int limit =
      request->limit() > 0 ? request->limit() : default_list_limit_;
  if (static_cast<int>(rows.size()) > limit) {
    rows.resize(limit);
  }
  for (auto &row : rows) {
    *response->add_candidates() = std::move(row);
  }
  return grpc::Status::OK;
}

auto ArenaService::Challenge(grpc::ServerContext * /*context*/,
                             const proto::ChallengeRequest *request,
                             proto::ChallengeResponse *response)
    -> grpc::Status {
  std::string error;
  const auto job_id = scheduler_->EnqueueChallenge(
      request->candidate_id(), request->opponent(), request->games(), &error);
  if (!job_id.has_value()) {
    return {grpc::StatusCode::INVALID_ARGUMENT, error};
  }
  response->set_job_id(*job_id);
  return grpc::Status::OK;
}

auto ArenaService::GetJob(grpc::ServerContext * /*context*/,
                          const proto::GetJobRequest *request,
                          proto::Job *response) -> grpc::Status {
  const auto job = scheduler_->GetJob(request->job_id());
  if (!job.has_value()) {
    return {grpc::StatusCode::NOT_FOUND,
            "unknown job '" + request->job_id() + "'"};
  }
  *response = *job;
  return grpc::Status::OK;
}

auto ArenaService::Leaderboard(grpc::ServerContext * /*context*/,
                               const proto::LeaderboardRequest *request,
                               proto::LeaderboardResponse *response)
    -> grpc::Status {
  std::vector<proto::CandidateStanding> rows;
  for (const proto::Candidate &candidate : candidates_->List()) {
    if (!request->game().empty() && candidate.game() != request->game()) {
      continue;
    }
    if (candidate.status() != proto::Candidate::READY) {
      continue;  // A leaderboard is for things that actually played.
    }
    rows.push_back(StandingFor(candidate));
  }
  std::sort(rows.begin(), rows.end(),
            [](const auto &a, const auto &b) { return a.elo() > b.elo(); });

  const int limit =
      request->limit() > 0 ? request->limit() : default_list_limit_;
  if (static_cast<int>(rows.size()) > limit) {
    rows.resize(limit);
  }
  for (auto &row : rows) {
    *response->add_rows() = std::move(row);
  }
  return grpc::Status::OK;
}

}  // namespace tournament_arena
