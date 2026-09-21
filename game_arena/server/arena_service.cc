#include "game_arena/server/arena_service.h"

#include <algorithm>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/log/log.h"

namespace tournament_arena {

ArenaService::ArenaService(CandidateStore *candidates, Scheduler *scheduler,
                           Standings *standings, bool graded, std::string game,
                           proto::ProblemInfo problem_info,
                           const ClientRegistry *clients,
                           int default_list_limit)
    : candidates_(candidates),
      scheduler_(scheduler),
      standings_(standings),
      graded_(graded),
      game_(std::move(game)),
      problem_info_(std::move(problem_info)),
      clients_(clients),
      default_list_limit_(default_list_limit) {}

bool ArenaService::Authenticate(grpc::ServerContext *context,
                                ClientIdentity *identity,
                                grpc::Status *status) const {
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

bool ArenaService::ResolveReader(grpc::ServerContext *context, bool strict,
                                 std::string *client_id,
                                 grpc::Status *status) const {
  if (problem_info_.source_visibility() != proto::ProblemInfo::SOURCE_OWN) {
    return true;
  }
  ClientIdentity identity;
  if (!Authenticate(context, &identity, status)) {
    // Lenient: an anonymous caller is nobody, which under this policy means
    // they own nothing and are served nothing of anyone's source. The rows
    // themselves are still theirs to read.
    return !strict;
  }
  *client_id = identity.client_id;
  return true;
}

bool ArenaService::MayReadSource(const proto::Candidate &candidate,
                                 const std::string &client_id) const {
  switch (problem_info_.source_visibility()) {
    case proto::ProblemInfo::SOURCE_NONE:
      return false;
    case proto::ProblemInfo::SOURCE_OWN:
      // An unauthenticated caller has no own candidates, and neither does a
      // candidate with no recorded author: no token, no match, no read.
      return !client_id.empty() && candidate.author() == client_id;
    default:
      return true;
  }
}

void ArenaService::RedactSource(proto::Candidate *candidate) const {
  candidate->clear_patch();
}

grpc::Status ArenaService::GetProblem(
    grpc::ServerContext * /*context*/,
    const proto::GetProblemRequest * /*request*/,
    proto::ProblemInfo *response) {
  *response = problem_info_;
  // Filled here rather than at startup: the score label belongs to the
  // standings, and asking them keeps one source of truth for it.
  response->set_score_label(standings_->score_label());
  response->set_graded(graded_);
  return grpc::Status::OK;
}

proto::CandidateStanding ArenaService::StandingFor(
    const proto::Candidate &candidate) const {
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

grpc::Status ArenaService::Submit(grpc::ServerContext *context,
                                  const proto::SubmitRequest *request,
                                  proto::SubmitResponse *response) {
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
  // One server runs one problem, so the game is the problem's, not the
  // submitter's: an empty one would otherwise reach the referee as --game="".
  if (attributed.game().empty()) {
    attributed.set_game(game_);
  }

  const auto candidate = candidates_->Create(attributed, &error);
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

grpc::Status ArenaService::GetCandidate(
    grpc::ServerContext *context, const proto::GetCandidateRequest *request,
    proto::Candidate *response) {
  std::string reader;
  grpc::Status status;
  if (!ResolveReader(context, /*strict=*/false, &reader, &status)) {
    return status;
  }
  const auto candidate = candidates_->Get(request->candidate_id());
  if (!candidate.has_value()) {
    return {grpc::StatusCode::NOT_FOUND,
            "unknown candidate '" + request->candidate_id() + "'"};
  }
  *response = *candidate;
  // The manifest is public -- who submitted what, and how it scored. The
  // patch on it is source, and follows the same rule GetSource does.
  if (!MayReadSource(*response, reader)) {
    RedactSource(response);
  }
  return grpc::Status::OK;
}

grpc::Status ArenaService::GetSource(grpc::ServerContext *context,
                                     const proto::GetSourceRequest *request,
                                     proto::SourceFile *response) {
  std::string reader;
  grpc::Status status;
  if (!ResolveReader(context, /*strict=*/true, &reader, &status)) {
    return status;
  }
  const auto candidate = candidates_->Get(request->candidate_id());
  if (!candidate.has_value()) {
    return {grpc::StatusCode::NOT_FOUND,
            "unknown candidate '" + request->candidate_id() + "'"};
  }
  if (!MayReadSource(*candidate, reader)) {
    // Named, not hidden: the candidate is on the leaderboard either way, and
    // "no such candidate" would only send an agent looking for a typo.
    return {grpc::StatusCode::PERMISSION_DENIED,
            problem_info_.source_visibility() == proto::ProblemInfo::SOURCE_OWN
                ? "this tournament serves only your own submissions' source"
                : "this tournament does not serve candidate source"};
  }
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

grpc::Status ArenaService::ListCandidates(
    grpc::ServerContext *context, const proto::ListCandidatesRequest *request,
    proto::ListCandidatesResponse *response) {
  std::string reader;
  grpc::Status status;
  if (!ResolveReader(context, /*strict=*/false, &reader, &status)) {
    return status;
  }
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
    if (!MayReadSource(row.candidate(), reader)) {
      RedactSource(row.mutable_candidate());
    }
    *response->add_candidates() = std::move(row);
  }
  return grpc::Status::OK;
}

grpc::Status ArenaService::GetJob(grpc::ServerContext * /*context*/,
                                  const proto::GetJobRequest *request,
                                  proto::Job *response) {
  const auto job = scheduler_->GetJob(request->job_id());
  if (!job.has_value()) {
    return {grpc::StatusCode::NOT_FOUND,
            "unknown job '" + request->job_id() + "'"};
  }
  *response = *job;
  return grpc::Status::OK;
}

grpc::Status ArenaService::Leaderboard(grpc::ServerContext *context,
                                       const proto::LeaderboardRequest *request,
                                       proto::LeaderboardResponse *response) {
  std::string reader;
  grpc::Status status;
  if (!ResolveReader(context, /*strict=*/false, &reader, &status)) {
    return status;
  }
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
    proto::CandidateStanding standing = StandingFor(*candidate);
    if (!MayReadSource(standing.candidate(), reader)) {
      RedactSource(standing.mutable_candidate());
    }
    *response->add_rows() = std::move(standing);
  }
  return grpc::Status::OK;
}

}  // namespace tournament_arena
