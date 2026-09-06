#include "game_arena/server/arena_service.h"

#include <algorithm>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/log/log.h"

namespace tournament_arena {

ArenaService::ArenaService(CandidateStore *candidates, Scheduler *scheduler,
                           Standings *standings, std::string base_commit,
                           bool graded, proto::ProblemInfo problem_info,
                           const ClientRegistry *clients,
                           int default_list_limit)
    : candidates_(candidates),
      scheduler_(scheduler),
      standings_(standings),
      base_commit_(std::move(base_commit)),
      graded_(graded),
      problem_info_(std::move(problem_info)),
      clients_(clients),
      default_list_limit_(default_list_limit) {}

auto ArenaService::Authenticate(grpc::ServerContext *context,
                                ClientIdentity *identity,
                                grpc::Status *status) const -> bool {
  if (clients_ == nullptr) {
    return true;  // no registry: nobody to authenticate, nothing to meter
  }
  const auto &metadata = context->client_metadata();
  const auto it = metadata.find("x-arena-token");
  if (it == metadata.end()) {
    *status = {grpc::StatusCode::UNAUTHENTICATED,
               "this arena requires a client token: send it as the "
               "x-arena-token metadata header. Ask the operator for one"};
    return false;
  }
  const auto resolved =
      clients_->Resolve(std::string_view(it->second.data(), it->second.size()));
  if (!resolved.has_value()) {
    // Deliberately the same message for "unknown" and "disabled": which one it
    // is tells a caller whether they have guessed a real token.
    *status = {grpc::StatusCode::UNAUTHENTICATED,
               "unknown or disabled client token"};
    return false;
  }
  *identity = *resolved;
  return true;
}

auto ArenaService::GetProblem(grpc::ServerContext * /*context*/,
                              const proto::GetProblemRequest * /*request*/,
                              proto::ProblemInfo *response) -> grpc::Status {
  *response = problem_info_;
  // Filled here rather than at startup: the score label belongs to the
  // standings, and asking them keeps one source of truth for it.
  response->set_score_label(standings_->score_label());
  response->set_base_commit(base_commit_);
  response->set_graded(graded_);
  return grpc::Status::OK;
}

auto ArenaService::StandingFor(const proto::Candidate &candidate) const
    -> proto::CandidateStanding {
  proto::CandidateStanding standing;
  *standing.mutable_candidate() = candidate;
  // Whatever this problem scores by. For a match problem that is ELO and W/D/L;
  // for a graded one, the ranked metric with the rest carried alongside.
  const Standing row = standings_->Get(candidate.candidate_id());
  standing.set_score(row.score);
  standing.set_wins(row.wins);
  standing.set_draws(row.draws);
  standing.set_losses(row.losses);
  standing.set_runs(row.runs);
  standing.set_worker_id(row.worker_id);
  standing.set_machine_class(row.machine_class);
  for (const auto &[name, value] : row.metrics) {
    (*standing.mutable_metrics())[name] = value;
  }
  return standing;
}

auto ArenaService::Submit(grpc::ServerContext *context,
                          const proto::SubmitRequest *request,
                          proto::SubmitResponse *response) -> grpc::Status {
  ClientIdentity identity;
  grpc::Status status;
  if (!Authenticate(context, &identity, &status)) {
    return status;
  }

  // Reserve before storing. A refused submission must leave nothing on disk,
  // and the check has to be part of the same locked step as the claim or two
  // concurrent submits both pass.
  std::string error;
  auto reservation = scheduler_->TryReserve(identity.client_id, identity.quota,
                                            request->cancel_running(), &error);
  if (!reservation.has_value()) {
    return {grpc::StatusCode::RESOURCE_EXHAUSTED, error};
  }

  // Attribution comes from the token, never the request: a quota you can
  // enforce beside a credit you cannot is only half a system.
  proto::SubmitRequest attributed = *request;
  if (!identity.client_id.empty()) {
    attributed.set_author(identity.client_id);
  }

  const auto candidate = candidates_->Create(attributed, base_commit_, &error);
  if (!candidate.has_value()) {
    // Rejections are the agent's to fix, so the message is the whole payload.
    // The reservation goes back when it falls out of scope here.
    return {grpc::StatusCode::INVALID_ARGUMENT, error};
  }
  response->set_candidate_id(candidate->candidate_id());
  for (const std::string &job_id : reservation->superseded()) {
    response->add_superseded_job_ids(job_id);
  }
  response->set_job_id(
      scheduler_->EnqueuePlacement(*candidate, std::move(*reservation)));
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
  const auto content =
      candidates_->ReadSource(request->candidate_id(), request->path(), &error);
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

  if (request->order() == proto::ListCandidatesRequest::BEST_FIRST) {
    // The standings already know which end is better, so take their order
    // rather than re-deriving it here and getting it backwards for a metric
    // where lower wins.
    std::vector<std::string> ranked;
    for (const Standing &row : standings_->Rank(0)) {
      ranked.push_back(row.candidate_id);
    }
    const auto rank_of = [&](const std::string &id) {
      const auto it = std::find(ranked.begin(), ranked.end(), id);
      return it == ranked.end() ? ranked.size()
                                : static_cast<std::size_t>(it - ranked.begin());
    };
    std::stable_sort(rows.begin(), rows.end(),
                     [&](const auto &a, const auto &b) {
                       return rank_of(a.candidate().candidate_id()) <
                              rank_of(b.candidate().candidate_id());
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

auto ArenaService::Evaluate(grpc::ServerContext *context,
                            const proto::EvaluateRequest *request,
                            proto::EvaluateResponse *response) -> grpc::Status {
  ClientIdentity identity;
  grpc::Status status;
  if (!Authenticate(context, &identity, &status)) {
    return status;
  }
  std::string error;
  auto reservation = scheduler_->TryReserve(identity.client_id, identity.quota,
                                            request->cancel_running(), &error);
  if (!reservation.has_value()) {
    return {grpc::StatusCode::RESOURCE_EXHAUSTED, error};
  }
  // Which shape is valid is the problem's, not the caller's. Saying so beats
  // defaulting: an agent that sends the wrong one learns the problem's shape
  // from the error rather than from a result that means nothing.
  if (graded_) {
    if (request->has_match()) {
      return {grpc::StatusCode::INVALID_ARGUMENT,
              "this problem is graded, not played: send `grade`, not `match`"};
    }
    const auto job_id = scheduler_->EnqueueRegrade(
        request->candidate_id(), request->grade().repeats(),
        std::move(*reservation), &error);
    if (!job_id.has_value()) {
      return {grpc::StatusCode::INVALID_ARGUMENT, error};
    }
    response->set_job_id(*job_id);
    return grpc::Status::OK;
  }

  if (request->has_grade()) {
    return {grpc::StatusCode::INVALID_ARGUMENT,
            "this problem is played, not graded: send `match`, not `grade`"};
  }
  const auto job_id = scheduler_->EnqueueChallenge(
      request->candidate_id(), request->match().opponent(),
      request->match().games(), std::move(*reservation), &error);
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

auto ArenaService::Leaderboard(
    grpc::ServerContext * /*context*/, const proto::LeaderboardRequest *request,
    proto::LeaderboardResponse *response) -> grpc::Status {
  const int limit =
      request->limit() > 0 ? request->limit() : default_list_limit_;
  // The ordering is the standings' to decide: lower is better for a runtime,
  // higher for a rating, and this has no business knowing which.
  response->set_score_label(standings_->score_label());
  for (const Standing &row : standings_->Rank(limit)) {
    const auto candidate = candidates_->Get(row.candidate_id);
    if (!candidate.has_value() ||
        candidate->status() != proto::Candidate::READY) {
      continue;  // A leaderboard is for things that actually ran.
    }
    *response->add_rows() = StandingFor(*candidate);
  }
  return grpc::Status::OK;
}

}  // namespace tournament_arena
