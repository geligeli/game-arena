#ifndef GAME_ARENA_GAME_ARENA_SERVER_ARENA_SERVICE_H
#define GAME_ARENA_GAME_ARENA_SERVER_ARENA_SERVICE_H

// Agent-facing RPCs. The token is metadata, so no stored request or log has it.

#include <string>

#include "game_arena/proto/arena.grpc.pb.h"
#include "game_arena/server/candidate_store.h"
#include "game_arena/server/client_registry.h"
#include "game_arena/server/scheduler.h"
#include "game_arena/standings/standings.h"

namespace tournament_arena {

class ArenaService final : public proto::Arena::Service {
 public:
  // A null |clients| leaves writes ungated.
  ArenaService(CandidateStore *candidates, Scheduler *scheduler,
               Standings *standings, std::string game,
               proto::ProblemInfo problem_info = {},
               ClientRegistry *clients = nullptr);

  grpc::Status Submit(grpc::ServerContext *context,
                      const proto::SubmitRequest *request,
                      proto::SubmitResponse *response) override;

  grpc::Status GetCandidate(grpc::ServerContext *context,
                            const proto::GetCandidateRequest *request,
                            proto::Candidate *response) override;

  grpc::Status GetSource(grpc::ServerContext *context,
                         const proto::GetSourceRequest *request,
                         proto::SourceFile *response) override;

  grpc::Status ListCandidates(grpc::ServerContext *context,
                              const proto::ListCandidatesRequest *request,
                              proto::ListCandidatesResponse *response) override;

  grpc::Status GetJob(grpc::ServerContext *context,
                      const proto::GetJobRequest *request,
                      proto::Job *response) override;

  grpc::Status Leaderboard(grpc::ServerContext *context,
                           const proto::LeaderboardRequest *request,
                           proto::LeaderboardResponse *response) override;

  grpc::Status GetProblem(grpc::ServerContext *context,
                          const proto::GetProblemRequest *request,
                          proto::ProblemInfo *response) override;

 private:
  proto::CandidateStanding StandingFor(const proto::Candidate &candidate) const;

  // With no registry it succeeds with an empty identity: nothing meters.
  bool Authenticate(grpc::ServerContext *context, ClientIdentity *identity,
                    grpc::Status *status) const;

  // SOURCE_OWN's caller, else empty. Lenient: no token gets redacted rows.
  std::string Reader(grpc::ServerContext *context) const;

  bool MayReadSource(const proto::Candidate &candidate,
                     const std::string &reader) const;

  // Clears the patch |reader| may not read; names and standings stay.
  void Redact(const std::string &reader, proto::Candidate *candidate) const;

  CandidateStore *candidates_;  // not owned
  Scheduler *scheduler_;        // not owned
  Standings *standings_;        // not owned
  const std::string game_;
  const proto::ProblemInfo problem_info_;
  ClientRegistry *clients_;  // not owned, may be null
};

}  // namespace tournament_arena

#endif  // GAME_ARENA_GAME_ARENA_SERVER_ARENA_SERVICE_H
