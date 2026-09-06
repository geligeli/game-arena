#ifndef RISK_GAME_AI_CPP_TOURNAMENT_SERVER_ARENA_SERVICE_H
#define RISK_GAME_AI_CPP_TOURNAMENT_SERVER_ARENA_SERVICE_H

// The agent-facing gRPC surface, consumed by the arena MCP server.
//
// Every RPC here is short and non-blocking: submitting stores files and
// queues work, it does not wait for a build. Agents poll GetJob instead, so a
// slow sandbox never holds an agent's tool call open.
//
// Writes are gated on an x-arena-token metadata header; reads are not. The
// leaderboard is meant to be public and readable source is the point of the
// arena, so GetSource, ListCandidates, GetJob, Leaderboard and GetProblem stay
// open. Only Submit and Evaluate spend the fleet, and only those are metered.
//
// The token is metadata rather than a request field so it never lands in a
// stored SubmitRequest, a manifest, or a log line -- and `author` is derived
// from it rather than self-reported, because a quota you can enforce beside a
// credit you cannot is only half a system.

#include <string>

#include "game_mcts/tournament_server/proto/arena.grpc.pb.h"
#include "game_mcts/tournament_server/server/candidate_store.h"
#include "game_mcts/tournament_server/server/client_registry.h"
#include "game_mcts/tournament_server/server/scheduler.h"
#include "game_mcts/tournament_server/server/standings.h"

namespace tournament_arena {

class ArenaService final : public proto::Arena::Service {
 public:
  // |base_commit| is the tree candidates are built against, recorded on each
  // submission so a rating stays attributable to a known revision.
  // |graded| says which shape EvaluateRequest must take, so an agent sending
  // the wrong one gets told rather than getting a default that means nothing.
  // |clients| may be null, which leaves writes ungated: a server with no
  // client registry has nobody to authenticate against. The startup log says
  // so, loudly.
  ArenaService(CandidateStore *candidates, Scheduler *scheduler,
               Standings *standings, std::string base_commit, bool graded,
               proto::ProblemInfo problem_info = {},
               const ClientRegistry *clients = nullptr,
               int default_list_limit = 50);

  auto Submit(grpc::ServerContext *context, const proto::SubmitRequest *request,
              proto::SubmitResponse *response) -> grpc::Status override;

  auto GetCandidate(grpc::ServerContext *context,
                    const proto::GetCandidateRequest *request,
                    proto::Candidate *response) -> grpc::Status override;

  auto GetSource(grpc::ServerContext *context,
                 const proto::GetSourceRequest *request,
                 proto::SourceFile *response) -> grpc::Status override;

  auto ListCandidates(
      grpc::ServerContext *context, const proto::ListCandidatesRequest *request,
      proto::ListCandidatesResponse *response) -> grpc::Status override;

  auto Evaluate(grpc::ServerContext *context,
                const proto::EvaluateRequest *request,
                proto::EvaluateResponse *response) -> grpc::Status override;

  auto GetJob(grpc::ServerContext *context, const proto::GetJobRequest *request,
              proto::Job *response) -> grpc::Status override;

  auto Leaderboard(
      grpc::ServerContext *context, const proto::LeaderboardRequest *request,
      proto::LeaderboardResponse *response) -> grpc::Status override;

  auto GetProblem(grpc::ServerContext *context,
                  const proto::GetProblemRequest *request,
                  proto::ProblemInfo *response) -> grpc::Status override;

 private:
  auto StandingFor(const proto::Candidate &candidate) const
      -> proto::CandidateStanding;

  // Resolves the caller's x-arena-token. Returns false with *status set when
  // the header is missing or names nobody. With no registry configured it
  // succeeds with an empty identity, and nothing downstream meters.
  auto Authenticate(grpc::ServerContext *context, ClientIdentity *identity,
                    grpc::Status *status) const -> bool;

  CandidateStore *candidates_;  // not owned
  Scheduler *scheduler_;        // not owned
  Standings *standings_;        // not owned
  const std::string base_commit_;
  const bool graded_;
  // Served verbatim by GetProblem; built once at startup from the config.
  const proto::ProblemInfo problem_info_;
  const ClientRegistry *clients_;  // not owned, may be null
  const int default_list_limit_;
};

}  // namespace tournament_arena

#endif  // RISK_GAME_AI_CPP_TOURNAMENT_SERVER_ARENA_SERVICE_H
