#include "game_arena/server/arena_service.h"

#include <algorithm>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/log/log.h"

namespace tournament_arena {

namespace {
constexpr int kDefaultListLimit = 50;
}  // namespace

ArenaService::ArenaService(CandidateStore *candidates, Scheduler *scheduler,
                           Standings *standings, std::string game,
                           proto::ProblemInfo problem_info,
                           ClientRegistry *clients)
    : candidates_(candidates),
      scheduler_(scheduler),
      standings_(standings),
      game_(std::move(game)),
      problem_info_(std::move(problem_info)),
      clients_(clients) {}

bool ArenaService::Authenticate(grpc::ServerContext *context,
                                ClientIdentity *identity,
                                grpc::Status *status) const {
  if (clients_ == nullptr) {
    return true;
  }
  const auto &metadata = context->client_metadata();
  const auto it = metadata.find("x-arena-token");
  if (it == metadata.end()) {
    *status = {grpc::StatusCode::UNAUTHENTICATED,
               "this arena requires a client token: send it as the "
               "x-arena-token metadata header. Ask the operator for one"};
    return false;
  }
  const std::string_view token(it->second.data(), it->second.size());
  auto resolved = clients_->Resolve(token);
  // A token minted since the registry was read is in the file already.
  if (!resolved.has_value()) {
    std::string error;
    if (clients_->Load(&error)) {
      resolved = clients_->Resolve(token);
    }
  }
  if (!resolved.has_value()) {
    // One message for both, or it would confirm a guessed token.
    *status = {grpc::StatusCode::UNAUTHENTICATED,
               "unknown or disabled client token"};
    return false;
  }
  *identity = *resolved;
  return true;
}

std::string ArenaService::Reader(grpc::ServerContext *context) const {
  ClientIdentity identity;
  grpc::Status ignored;
  if (problem_info_.source_visibility() == proto::ProblemInfo::SOURCE_OWN) {
    Authenticate(context, &identity, &ignored);
  }
  return identity.client_id;
}

bool ArenaService::MayReadSource(const proto::Candidate &candidate,
                                 const std::string &reader) const {
  switch (problem_info_.source_visibility()) {
    case proto::ProblemInfo::SOURCE_NONE:
      return false;
    case proto::ProblemInfo::SOURCE_OWN:
      // Empty must not match an authorless candidate: no token, no read.
      return !reader.empty() && candidate.author() == reader;
    default:
      return true;
  }
}

void ArenaService::Redact(const std::string &reader,
                          proto::Candidate *candidate) const {
  if (!MayReadSource(*candidate, reader)) {
    candidate->clear_patch();
  }
}

grpc::Status ArenaService::GetProblem(
    grpc::ServerContext * /*context*/,
    const proto::GetProblemRequest * /*request*/,
    proto::ProblemInfo *response) {
  *response = problem_info_;
  response->set_score_label(standings_->score_label());
  return grpc::Status::OK;
}

proto::CandidateStanding ArenaService::StandingFor(
    const proto::Candidate &candidate) const {
  proto::CandidateStanding standing;
  *standing.mutable_candidate() = candidate;
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

  // Reserve before storing, so a refused submission leaves nothing on disk.
  std::string error;
  auto reservation = scheduler_->TryReserve(identity.client_id, identity.quota,
                                            request->cancel_running(), &error);
  if (!reservation.has_value()) {
    return {grpc::StatusCode::RESOURCE_EXHAUSTED, error};
  }

  // Attribution comes from the token, never the request.
  proto::SubmitRequest attributed = *request;
  if (!identity.client_id.empty()) {
    attributed.set_author(identity.client_id);
    if (attributed.display_name().empty()) {
      attributed.set_display_name(identity.client_id);
    }
  }
  // The game is the problem's; an empty one would reach the referee as "".
  if (attributed.game().empty()) {
    attributed.set_game(game_);
  }

  const auto candidate = candidates_->Create(attributed, &error);
  if (!candidate.has_value()) {
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

std::optional<proto::Candidate> ArenaService::Resolve(
    const std::string &id) const {
  if (auto candidate = candidates_->Get(id)) {
    return candidate;
  }
  // With versions a participant's name is no candidate, but it stands for
  // their newest version that built, under the name: the id, and paths in
  // <submit_dir>/<name>/. A kit restores it, and `source` and `spar` pull it,
  // exactly where they always did.
  std::optional<proto::Candidate> latest;
  if (candidates_->versions()) {
    latest = candidates_->Latest(id);
  }
  if (!latest.has_value()) {
    return std::nullopt;
  }
  const std::string &dir = problem_info_.files_submit_dir();
  const std::string version = latest->candidate_id();
  latest->set_candidate_id(id);
  latest->set_patch(MovePatchDir(latest->patch(), dir, version, id));
  for (std::string &path : *latest->mutable_file_paths()) {
    path = MovePatchDir(path, dir, version, id);
  }
  for (std::string &path : *latest->mutable_touched_paths()) {
    path = MovePatchDir(path, dir, version, id);
  }
  return latest;
}

grpc::Status ArenaService::GetCandidate(
    grpc::ServerContext *context, const proto::GetCandidateRequest *request,
    proto::Candidate *response) {
  const std::string reader = Reader(context);
  const auto candidate = Resolve(request->candidate_id());
  if (!candidate.has_value()) {
    return {grpc::StatusCode::NOT_FOUND,
            "unknown candidate '" + request->candidate_id() + "'"};
  }
  *response = *candidate;
  Redact(reader, response);
  return grpc::Status::OK;
}

grpc::Status ArenaService::GetSource(grpc::ServerContext *context,
                                     const proto::GetSourceRequest *request,
                                     proto::SourceFile *response) {
  // Strict, unlike the listings: an anonymous caller is UNAUTHENTICATED.
  ClientIdentity reader;
  grpc::Status status;
  if (problem_info_.source_visibility() == proto::ProblemInfo::SOURCE_OWN &&
      !Authenticate(context, &reader, &status)) {
    return status;
  }
  const auto candidate = Resolve(request->candidate_id());
  if (!candidate.has_value()) {
    return {grpc::StatusCode::NOT_FOUND,
            "unknown candidate '" + request->candidate_id() + "'"};
  }
  if (!MayReadSource(*candidate, reader.client_id)) {
    // Not NOT_FOUND: the candidate is on the leaderboard either way.
    return {grpc::StatusCode::PERMISSION_DENIED,
            problem_info_.source_visibility() == proto::ProblemInfo::SOURCE_OWN
                ? "this tournament serves only your own submissions' source"
                : "this tournament does not serve candidate source"};
  }
  // A name's paths are its version's, moved: back to where they are stored.
  std::string stored = request->candidate_id();
  std::string path = request->path();
  if (!candidates_->Get(stored).has_value()) {
    const std::string version =
        candidates_->Latest(request->candidate_id())->candidate_id();
    path =
        MovePatchDir(path, problem_info_.files_submit_dir(), stored, version);
    stored = version;
  }
  std::string error;
  const auto content = candidates_->ReadSource(stored, path, &error);
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
  const std::string reader = Reader(context);
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
    // The standings know which end is better; for some metrics lower wins.
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

  const int limit = request->limit() > 0 ? request->limit() : kDefaultListLimit;
  if (static_cast<int>(rows.size()) > limit) {
    rows.resize(limit);
  }
  for (auto &row : rows) {
    Redact(reader, row.mutable_candidate());
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
  const std::string reader = Reader(context);
  const int limit = request->limit() > 0 ? request->limit() : kDefaultListLimit;
  response->set_score_label(standings_->score_label());
  response->set_graded(standings_->graded());
  for (const Standing &row : standings_->Rank(limit)) {
    const auto candidate = candidates_->Get(row.candidate_id);
    if (!candidate.has_value() ||
        candidate->status() != proto::Candidate::READY) {
      continue;
    }
    proto::CandidateStanding standing = StandingFor(*candidate);
    Redact(reader, standing.mutable_candidate());
    *response->add_rows() = std::move(standing);
  }
  return grpc::Status::OK;
}

}  // namespace tournament_arena
