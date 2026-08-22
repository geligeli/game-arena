#ifndef RISK_GAME_AI_CPP_TOURNAMENT_SERVER_ARENA_SERVICE_H
#define RISK_GAME_AI_CPP_TOURNAMENT_SERVER_ARENA_SERVICE_H

// The agent-facing gRPC surface, consumed by the arena MCP server.
//
// Every RPC here is short and non-blocking: submitting stores files and
// queues work, it does not wait for a build. Agents poll GetJob instead, so a
// slow sandbox never holds an agent's tool call open.

#include <string>

#include "game_mcts/tournament_server/arena.grpc.pb.h"
#include "game_mcts/tournament_server/candidate_store.h"
#include "game_mcts/tournament_server/elo_store.h"
#include "game_mcts/tournament_server/scheduler.h"

namespace tournament_arena {

class ArenaService final : public proto::Arena::Service {
 public:
  // |base_commit| is the tree candidates are built against, recorded on each
  // submission so a rating stays attributable to a known revision.
  ArenaService(CandidateStore *candidates, Scheduler *scheduler,
               tournament_broker::EloStore *elo_store, std::string base_commit,
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

  auto Challenge(grpc::ServerContext *context,
                 const proto::ChallengeRequest *request,
                 proto::ChallengeResponse *response) -> grpc::Status override;

  auto GetJob(grpc::ServerContext *context, const proto::GetJobRequest *request,
              proto::Job *response) -> grpc::Status override;

  auto Leaderboard(
      grpc::ServerContext *context, const proto::LeaderboardRequest *request,
      proto::LeaderboardResponse *response) -> grpc::Status override;

 private:
  auto StandingFor(const proto::Candidate &candidate) const
      -> proto::CandidateStanding;

  CandidateStore *candidates_;              // not owned
  Scheduler *scheduler_;                    // not owned
  tournament_broker::EloStore *elo_store_;  // not owned
  const std::string base_commit_;
  const int default_list_limit_;
};

}  // namespace tournament_arena

#endif  // RISK_GAME_AI_CPP_TOURNAMENT_SERVER_ARENA_SERVICE_H
