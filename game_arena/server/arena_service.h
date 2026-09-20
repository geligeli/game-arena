#ifndef GAME_ARENA_GAME_ARENA_SERVER_ARENA_SERVICE_H
#define GAME_ARENA_GAME_ARENA_SERVER_ARENA_SERVICE_H

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

#include "game_arena/proto/arena.grpc.pb.h"
#include "game_arena/server/candidate_store.h"
#include "game_arena/server/client_registry.h"
#include "game_arena/server/scheduler.h"
#include "game_arena/standings/standings.h"

namespace tournament_arena {

class ArenaService final : public proto::Arena::Service {
 public:
  // |graded| says which shape EvaluateRequest must take, so an agent sending
  // the wrong one gets told rather than getting a default that means nothing.
  // |game| is the problem's game (empty for a graded problem): one server
  // runs one problem, so a submission that does not name a game gets this
  // one -- the game is not the submitter's to state.
  // |clients| may be null, which leaves writes ungated: a server with no
  // client registry has nobody to authenticate against. The startup log says
  // so, loudly.
  ArenaService(CandidateStore *candidates, Scheduler *scheduler,
               Standings *standings, bool graded, std::string game,
               proto::ProblemInfo problem_info = {},
               const ClientRegistry *clients = nullptr,
               int default_list_limit = 50);

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

  grpc::Status Evaluate(grpc::ServerContext *context,
                        const proto::EvaluateRequest *request,
                        proto::EvaluateResponse *response) override;

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

  // Resolves the caller's x-arena-token. Returns false with *status set when
  // the header is missing or names nobody. With no registry configured it
  // succeeds with an empty identity, and nothing downstream meters.
  bool Authenticate(grpc::ServerContext *context, ClientIdentity *identity,
                    grpc::Status *status) const;

  // Who the caller is, for the problem's source policy. Only SOURCE_OWN needs
  // to know: it fills *client_id from the caller's token, and under every
  // other policy nobody is asked and *client_id stays empty.
  //
  // |strict| is for the calls that serve source and nothing else: a missing
  // or unknown token fails there, because "you are nobody, so you may read
  // nothing" is worth saying as UNAUTHENTICATED. The listings are lenient --
  // a standing is not source, and a leaderboard should stay legible to
  // someone who has not been given a token at all. They redact instead.
  bool ResolveReader(grpc::ServerContext *context, bool strict,
                     std::string *client_id, grpc::Status *status) const;

  // Whether |candidate|'s files may be served to |client_id|.
  bool MayReadSource(const proto::Candidate &candidate,
                     const std::string &client_id) const;

  // Drops the source bytes from a manifest the caller may not read, leaving
  // the names and the standings: who submitted what, without the what.
  void RedactSource(proto::Candidate *candidate) const;

  CandidateStore *candidates_;  // not owned
  Scheduler *scheduler_;        // not owned
  Standings *standings_;        // not owned
  const bool graded_;
  const std::string game_;
  // Served verbatim by GetProblem; built once at startup from the config.
  const proto::ProblemInfo problem_info_;
  const ClientRegistry *clients_;  // not owned, may be null
  const int default_list_limit_;
};

}  // namespace tournament_arena

#endif  // GAME_ARENA_GAME_ARENA_SERVER_ARENA_SERVICE_H
