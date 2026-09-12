// Command-line client for the arena's Arena service.
/*
bazel run //game_arena/tools:arena_cli -- \
    --server=localhost:50051 rules

bazel run //game_arena/tools:arena_cli -- \
    submit --name="My Bot" --file=strategy.h --wait

bazel run //game_arena/tools:arena_cli -- \
    submit --name="My Bot" --patch=my.diff

bazel run //game_arena/tools:arena_cli -- job <job_id> [--wait]
bazel run //game_arena/tools:arena_cli -- candidates [--order=newest]
bazel run //game_arena/tools:arena_cli -- leaderboard [--limit=20]
bazel run //game_arena/tools:arena_cli -- source <candidate_id> [path]
bazel run //game_arena/tools:arena_cli -- evaluate <candidate_id> \
    [--opponent=ladder|--repeats=5] [--wait]
*/
//
// The human's counterpart to the MCP server: same RPCs, same compact output,
// drivable from a shell. Submit and Evaluate are the write path and carry the
// --token as x-arena-token metadata; everything else is a read and open.

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "absl/flags/flag.h"
#include "absl/flags/parse.h"
#include "absl/log/globals.h"
#include "absl/log/initialize.h"
#include "game_arena/proto/arena.grpc.pb.h"
#include <grpcpp/grpcpp.h>

ABSL_FLAG(std::string, server, "",
          "Arena address (default: $ARENA_SERVER, else localhost:50051)");
ABSL_FLAG(std::string, token, "",
          "Bearer token for Submit/Evaluate, sent as the x-arena-token "
          "metadata header (default: $ARENA_TOKEN)");
ABSL_FLAG(int, timeout_s, 120, "Per-RPC timeout in seconds");

ABSL_FLAG(std::string, name, "",
          "submit: display name for the candidate (required)");
ABSL_FLAG(std::vector<std::string>, file, {},
          "submit: a source file to submit (repeatable). Read from disk, "
          "flattened to its basename; the server generates the BUILD");
ABSL_FLAG(std::string, patch, "",
          "submit: a unified diff against the problem's base_commit, read "
          "from disk. Mutually exclusive with --file");
ABSL_FLAG(std::string, entry_header, "",
          "submit: the header defining MakePolicy. Defaults to the sole "
          ".h/.hpp among --file");
ABSL_FLAG(std::string, parent_id, "",
          "submit: the candidate this one builds on, for lineage");
ABSL_FLAG(std::string, notes, "", "submit: free-form notes");
ABSL_FLAG(std::vector<std::string>, param, {},
          "submit: a k=v registry parameter (repeatable)");
ABSL_FLAG(std::vector<std::string>, extra_dep, {},
          "submit: an extra bazel dep within the problem's allowlist "
          "(repeatable)");
ABSL_FLAG(std::string, game, "",
          "submit/candidates/leaderboard: restrict to this game "
          "(default: the server's)");
ABSL_FLAG(bool, cancel_running, false,
          "submit/evaluate: abort this client's in-flight work and take its "
          "place, instead of being refused for being over quota");
ABSL_FLAG(bool, wait, false,
          "submit/job/evaluate: poll until the job reaches a terminal state");

ABSL_FLAG(std::string, author, "", "candidates: restrict to this author");
ABSL_FLAG(std::string, order, "best", "candidates: best | newest");
ABSL_FLAG(int, limit, 20, "candidates/leaderboard: max rows (0: default)");

ABSL_FLAG(std::string, opponent, "",
          "evaluate: builtin:<spec> | <candidate_id> | top | ladder");
ABSL_FLAG(int, games, 0, "evaluate: match games (0: the problem's default)");
ABSL_FLAG(int, repeats, 0,
          "evaluate: extra measurement runs, for a graded problem");

namespace {

namespace proto = tournament_arena::proto;

constexpr int kPollIntervalS = 2;
constexpr int kExitError = 1;
constexpr int kExitUsage = 2;

auto EnvOr(const char *name, std::string fallback) -> std::string {
  if (const char *value = std::getenv(name); value != nullptr) {
    return value;
  }
  return fallback;
}

struct Client {
  std::unique_ptr<proto::Arena::Stub> stub;
  std::string token;
  int timeout_s;
};

void ConfigureContext(const Client &client, bool write,
                      grpc::ClientContext *context) {
  context->set_deadline(std::chrono::system_clock::now() +
                        std::chrono::seconds(client.timeout_s));
  if (write && !client.token.empty()) {
    context->AddMetadata("x-arena-token", client.token);
  }
}

// Prints a mapped error to stderr and returns the process exit code. Mirrors
// _rpc_error in the MCP server.
auto RpcError(const grpc::Status &status, const std::string &server) -> int {
  switch (status.error_code()) {
    case grpc::StatusCode::UNAUTHENTICATED:
      std::fprintf(stderr,
                   "ERROR: %s\n"
                   "Set --token (or ARENA_TOKEN). The arena's operator mints "
                   "one with:\n"
                   "  bazel run //game_arena/tools:arena_admin -- "
                   "mint --client_id=<you>\n",
                   status.error_message().c_str());
      return kExitError;
    case grpc::StatusCode::RESOURCE_EXHAUSTED:
      std::fprintf(stderr,
                   "ERROR: over quota. %s\n"
                   "Poll `job` until the running one finishes, or pass "
                   "--cancel_running to replace it.\n",
                   status.error_message().c_str());
      return kExitError;
    case grpc::StatusCode::UNAVAILABLE:
      std::fprintf(stderr,
                   "ERROR: no arena at %s. Start it with:\n"
                   "  bazel run //game_arena/server:problem_server -- "
                   "--problem_config=<problem>.textproto "
                   "--data_dir=tournament_data\n",
                   server.c_str());
      return kExitError;
    default:
      std::fprintf(stderr, "ERROR: %s\n", status.error_message().c_str());
      return kExitError;
  }
}

auto StatusName(proto::Candidate::Status status) -> const char * {
  switch (status) {
    case proto::Candidate::PENDING:
      return "pending";
    case proto::Candidate::BUILDING:
      return "building";
    case proto::Candidate::READY:
      return "ready";
    case proto::Candidate::BUILD_FAILED:
      return "build-failed";
    case proto::Candidate::DISABLED:
      return "disabled";
    default:
      return "?";
  }
}

auto JobStateName(proto::Job::State state) -> const char * {
  switch (state) {
    case proto::Job::QUEUED:
      return "queued";
    case proto::Job::RUNNING:
      return "running";
    case proto::Job::DONE:
      return "done";
    case proto::Job::FAILED:
      return "failed";
    case proto::Job::CANCELLED:
      return "cancelled";
    default:
      return "?";
  }
}

// How far a running job has got. Worth showing because a build can take half
// an hour: "running" on its own does not tell you whether to keep waiting.
auto PhaseName(proto::OrderProgress::Phase phase) -> const char * {
  switch (phase) {
    case proto::OrderProgress::CLONING:
      return "cloning";
    case proto::OrderProgress::BUILDING:
      return "building";
    case proto::OrderProgress::RUNNING:
      return "running";
    default:
      return "?";
  }
}

auto IsTerminal(proto::Job::State state) -> bool {
  return state == proto::Job::DONE || state == proto::Job::FAILED ||
         state == proto::Job::CANCELLED;
}

// One leaderboard/candidates row, rendered for whichever kind of problem this
// is: a measurement is not a property of the submission alone, so the host
// that produced it belongs on the row.
void PrintStandingHeader(const std::string &score_label, bool graded) {
  if (graded) {
    std::printf("%-28s %9s %5s %-12s %-12s %-12s %s\n", "candidate_id",
                score_label.empty() ? "score" : score_label.c_str(), "runs",
                "machine", "status", "author", "name");
  } else {
    std::printf("%-28s %9s %11s %5s %-12s %-12s %s\n", "candidate_id",
                score_label.empty() ? "score" : score_label.c_str(), "W/D/L",
                "games", "status", "author", "name");
  }
}

void PrintStandingRow(const proto::CandidateStanding &standing, bool graded) {
  const proto::Candidate &candidate = standing.candidate();
  std::printf("%-28s %9.3f ", candidate.candidate_id().c_str(),
              standing.score());
  if (graded) {
    std::printf("%4dr %-12s ", standing.runs(),
                standing.machine_class().empty()
                    ? "-"
                    : standing.machine_class().c_str());
  } else {
    const int played = standing.wins() + standing.draws() + standing.losses();
    std::printf("%3d/%3d/%3d %4dg ", standing.wins(), standing.draws(),
                standing.losses(), played);
  }
  std::printf("%-12s %-12s %s", StatusName(candidate.status()),
              candidate.author().empty() ? "-" : candidate.author().c_str(),
              candidate.display_name().c_str());
  if (!candidate.parent_id().empty()) {
    std::printf("  <- %s", candidate.parent_id().c_str());
  }
  std::printf("\n");
}

void PrintJob(const proto::Job &job) {
  // The phase only means anything while the job is still going; once it is
  // done, the last phase it reached is noise.
  const std::string state =
      job.state() == proto::Job::RUNNING
          ? std::string(JobStateName(job.state())) + ", " +
                PhaseName(job.phase())
          : JobStateName(job.state());
  std::printf("job %s [%s] candidate %s\n", job.job_id().c_str(),
              state.c_str(), job.candidate_id().c_str());
  std::printf("games %d/%d  W/D/L %d/%d/%d  score %.3f\n", job.games_played(),
              job.games_requested(), job.wins(), job.draws(), job.losses(),
              job.elo());
  if (!job.error().empty()) {
    std::printf("\n%s\n", job.error().c_str());
  }
}

// Polls until the job reaches a terminal state, printing state transitions.
// Returns the exit code: 0 on DONE, 1 on FAILED/CANCELLED or an RPC error.
auto WaitForJob(const Client &client, const std::string &server,
                const std::string &job_id) -> int {
  proto::Job::State last = proto::Job::QUEUED;
  // Tracked alongside the state so a long build reports cloning, then
  // building, then running, instead of one "running" line for half an hour.
  std::optional<proto::OrderProgress::Phase> last_phase;
  for (;;) {
    proto::GetJobRequest request;
    request.set_job_id(job_id);
    proto::Job job;
    grpc::ClientContext context;
  ConfigureContext(client, /*write=*/false, &context);
    const grpc::Status status =
        client.stub->GetJob(&context, request, &job);
    if (!status.ok()) {
      return RpcError(status, server);
    }
    if (job.state() != last || last_phase != job.phase()) {
      PrintJob(job);
      last = job.state();
      last_phase = job.phase();
    }
    if (IsTerminal(job.state())) {
      return job.state() == proto::Job::DONE ? 0 : kExitError;
    }
    std::this_thread::sleep_for(std::chrono::seconds(kPollIntervalS));
  }
}

auto CmdRules(const Client &client, const std::string &server) -> int {
  grpc::ClientContext context;
  ConfigureContext(client, /*write=*/false, &context);
  proto::ProblemInfo problem;
  const grpc::Status status =
      client.stub->GetProblem(&context, proto::GetProblemRequest(), &problem);
  if (!status.ok()) {
    return RpcError(status, server);
  }

  std::printf("PROBLEM  %s  [%s]\n",
              problem.display_name().empty() ? problem.problem_id().c_str()
                                             : problem.display_name().c_str(),
              problem.problem_id().c_str());
  std::printf("  scored by %s", problem.score_label().c_str());
  if (problem.graded()) {
    std::printf(", %s is better\n",
                problem.lower_is_better() ? "lower" : "higher");
  } else {
    std::printf(" (play others and be rated)\n");
  }
  std::printf("  built against base_commit %s\n\n",
              problem.base_commit().empty() ? "-" : problem.base_commit().c_str());
  if (!problem.description().empty()) {
    std::printf("%s\n\n", problem.description().c_str());
  }
  std::printf("SUBMITTING\n");
  if (!problem.files_submit_dir().empty()) {
    std::printf(
        "  files:  submit --name=... --file=strategy.h\n"
        "          (placed under %s/<your-id>/, BUILD generated)\n"
        "  patch:  submit --name=... --patch=my.diff\n",
        problem.files_submit_dir().c_str());
  } else {
    std::printf(
        "  patch only: submit --name=... --patch=my.diff\n"
        "  (this problem's solutions change existing code)\n");
  }
  std::printf("  then poll: job <job_id> [--wait]\n\n");
  std::printf("LIMITS\n  patch <= %llu bytes, %u files, %u hunks\n",
              static_cast<unsigned long long>(problem.max_patch_bytes()),
              problem.max_files(), problem.max_hunks());
  for (const std::string &allow : problem.allow_paths()) {
    std::printf("  may touch   %s\n", allow.c_str());
  }
  for (const std::string &deny : problem.deny_paths()) {
    std::printf("  may not     %s\n", deny.c_str());
  }
  return 0;
}

auto ReadFile(const std::string &path, std::string *content) -> bool {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) {
    return false;
  }
  std::ostringstream buffer;
  buffer << stream.rdbuf();
  *content = buffer.str();
  return true;
}

auto CmdSubmit(const Client &client, const std::string &server) -> int {
  const std::vector<std::string> files = absl::GetFlag(FLAGS_file);
  const std::string patch_path = absl::GetFlag(FLAGS_patch);
  const std::string name = absl::GetFlag(FLAGS_name);

  if (name.empty()) {
    std::fprintf(stderr, "submit: --name is required\n");
    return kExitUsage;
  }
  if (files.empty() == patch_path.empty()) {
    std::fprintf(stderr,
                 "submit: pass either --file (repeatable) or --patch, not "
                 "both, not neither (see `rules` for which this problem "
                 "takes)\n");
    return kExitUsage;
  }

  proto::SubmitRequest request;
  request.set_display_name(name);
  request.set_game(absl::GetFlag(FLAGS_game));
  request.set_parent_id(absl::GetFlag(FLAGS_parent_id));
  request.set_notes(absl::GetFlag(FLAGS_notes));
  request.set_cancel_running(absl::GetFlag(FLAGS_cancel_running));

  if (!patch_path.empty()) {
    std::string patch;
    if (!ReadFile(patch_path, &patch)) {
      std::fprintf(stderr, "submit: cannot read patch %s\n",
                   patch_path.c_str());
      return kExitUsage;
    }
    request.set_patch(std::move(patch));
  } else {
    // Flattened to the basename: a submission is its own directory, so the
    // sender's layout above it is irrelevant and only invites "..".
    std::vector<std::string> headers;
    for (const std::string &raw : files) {
      std::string content;
      if (!ReadFile(raw, &content)) {
        std::fprintf(stderr, "submit: cannot read %s\n", raw.c_str());
        return kExitUsage;
      }
      const std::filesystem::path path(raw);
      proto::SourceFile *file = request.add_files();
      file->set_path(path.filename().string());
      file->set_content(std::move(content));
      const std::string extension = path.extension().string();
      if (extension == ".h" || extension == ".hpp") {
        headers.push_back(path.filename().string());
      }
    }
    std::string entry_header = absl::GetFlag(FLAGS_entry_header);
    if (entry_header.empty()) {
      if (headers.size() == 1) {
        entry_header = headers.front();
      } else {
        std::string candidates;
        for (const std::string &header : headers) {
          candidates += (candidates.empty() ? "" : ", ") + header;
        }
        std::fprintf(stderr,
                     "submit: --entry_header is required when the submission "
                     "has %zu headers (candidates: %s)\n",
                     headers.size(),
                     candidates.empty() ? "none" : candidates.c_str());
        return kExitUsage;
      }
    }
    request.set_entry_header(std::filesystem::path(entry_header)
                                 .filename()
                                 .string());
    for (const std::string &kv : absl::GetFlag(FLAGS_param)) {
      const auto eq = kv.find('=');
      if (eq == std::string::npos) {
        std::fprintf(stderr, "submit: --param expects k=v, got '%s'\n",
                     kv.c_str());
        return kExitUsage;
      }
      (*request.mutable_params())[kv.substr(0, eq)] = kv.substr(eq + 1);
    }
    for (const std::string &dep : absl::GetFlag(FLAGS_extra_dep)) {
      request.add_extra_deps(dep);
    }
  }

  grpc::ClientContext context;
  ConfigureContext(client, /*write=*/true, &context);
  proto::SubmitResponse response;
  const grpc::Status status = client.stub->Submit(&context, request, &response);
  if (!status.ok()) {
    return RpcError(status, server);
  }

  std::printf("candidate %s\njob %s queued (build + placement)\n",
              response.candidate_id().c_str(), response.job_id().c_str());
  for (const std::string &superseded : response.superseded_job_ids()) {
    std::printf("superseded job %s (cancelled)\n", superseded.c_str());
  }
  if (!absl::GetFlag(FLAGS_wait)) {
    std::printf("poll: job %s [--wait]\n", response.job_id().c_str());
    return 0;
  }
  return WaitForJob(client, server, response.job_id());
}

auto CmdJob(const Client &client, const std::string &server,
            const std::vector<char *> &args) -> int {
  if (args.empty()) {
    std::fprintf(stderr, "job: a job id is required\n");
    return kExitUsage;
  }
  if (absl::GetFlag(FLAGS_wait)) {
    return WaitForJob(client, server, args[0]);
  }
  proto::GetJobRequest request;
  request.set_job_id(args[0]);
  proto::Job job;
  grpc::ClientContext context;
  ConfigureContext(client, /*write=*/false, &context);
  const grpc::Status status = client.stub->GetJob(&context, request, &job);
  if (!status.ok()) {
    return RpcError(status, server);
  }
  PrintJob(job);
  return IsTerminal(job.state()) && job.state() != proto::Job::DONE
             ? kExitError
             : 0;
}

auto CmdCandidates(const Client &client, const std::string &server) -> int {
  proto::ListCandidatesRequest request;
  request.set_game(absl::GetFlag(FLAGS_game));
  request.set_author(absl::GetFlag(FLAGS_author));
  request.set_limit(absl::GetFlag(FLAGS_limit));
  const std::string order = absl::GetFlag(FLAGS_order);
  if (order == "newest") {
    request.set_order(proto::ListCandidatesRequest::NEWEST);
  } else if (order == "best") {
    request.set_order(proto::ListCandidatesRequest::BEST_FIRST);
  } else {
    std::fprintf(stderr, "candidates: --order is best | newest, got '%s'\n",
                 order.c_str());
    return kExitUsage;
  }

  grpc::ClientContext context;
  ConfigureContext(client, /*write=*/false, &context);
  proto::ListCandidatesResponse response;
  const grpc::Status status =
      client.stub->ListCandidates(&context, request, &response);
  if (!status.ok()) {
    return RpcError(status, server);
  }
  if (response.candidates().empty()) {
    std::printf("no candidates yet\n");
    return 0;
  }
  // This listing has no score_label of its own; a row carrying metrics is a
  // graded one.
  const bool graded =
      std::any_of(response.candidates().begin(), response.candidates().end(),
                  [](const proto::CandidateStanding &standing) {
                    return !standing.metrics().empty();
                  });
  PrintStandingHeader("score", graded);
  for (const proto::CandidateStanding &standing : response.candidates()) {
    PrintStandingRow(standing, graded);
  }
  return 0;
}

auto CmdLeaderboard(const Client &client, const std::string &server) -> int {
  proto::LeaderboardRequest request;
  request.set_game(absl::GetFlag(FLAGS_game));
  request.set_limit(absl::GetFlag(FLAGS_limit));
  grpc::ClientContext context;
  ConfigureContext(client, /*write=*/false, &context);
  proto::LeaderboardResponse response;
  const grpc::Status status =
      client.stub->Leaderboard(&context, request, &response);
  if (!status.ok()) {
    return RpcError(status, server);
  }
  if (response.rows().empty()) {
    std::printf("nothing has been scored yet\n");
    return 0;
  }
  // The server says what its score column means; a graded problem's is a
  // metric name, not "elo".
  const bool graded =
      response.score_label() != "" && response.score_label() != "elo";
  PrintStandingHeader(response.score_label(), graded);
  for (const proto::CandidateStanding &standing : response.rows()) {
    PrintStandingRow(standing, graded);
  }
  return 0;
}

auto CmdSource(const Client &client, const std::string &server,
               const std::vector<char *> &args) -> int {
  if (args.empty()) {
    std::fprintf(stderr, "source: a candidate id is required\n");
    return kExitUsage;
  }
  const std::string candidate_id = args[0];
  if (args.size() > 1) {
    proto::GetSourceRequest request;
    request.set_candidate_id(candidate_id);
    request.set_path(args[1]);
    grpc::ClientContext context;
  ConfigureContext(client, /*write=*/false, &context);
    proto::SourceFile source;
    const grpc::Status status =
        client.stub->GetSource(&context, request, &source);
    if (!status.ok()) {
      return RpcError(status, server);
    }
    std::fwrite(source.content().data(), 1, source.content().size(), stdout);
    return 0;
  }

  proto::GetCandidateRequest request;
  request.set_candidate_id(candidate_id);
  grpc::ClientContext context;
  ConfigureContext(client, /*write=*/false, &context);
  proto::Candidate candidate;
  const grpc::Status status =
      client.stub->GetCandidate(&context, request, &candidate);
  if (!status.ok()) {
    return RpcError(status, server);
  }
  std::printf("%s  \"%s\"\n", candidate.candidate_id().c_str(),
              candidate.display_name().c_str());
  std::printf("author %s  game %s  status %s\n",
              candidate.author().empty() ? "-" : candidate.author().c_str(),
              candidate.game().empty() ? "-" : candidate.game().c_str(),
              StatusName(candidate.status()));
  std::printf("base_commit %s  parent %s\n",
              candidate.base_commit().empty() ? "-"
                                              : candidate.base_commit().c_str(),
              candidate.parent_id().empty() ? "-"
                                            : candidate.parent_id().c_str());
  if (!candidate.entry_header().empty()) {
    std::printf("entry_header %s\n", candidate.entry_header().c_str());
  }
  if (!candidate.params().empty()) {
    std::printf("params ");
    std::string separator;
    for (const auto &[key, value] : candidate.params()) {
      std::printf("%s%s=%s", separator.c_str(), key.c_str(), value.c_str());
      separator = ",";
    }
    std::printf("\n");
  }
  if (!candidate.extra_deps().empty()) {
    std::printf("extra_deps");
    for (const std::string &dep : candidate.extra_deps()) {
      std::printf(" %s", dep.c_str());
    }
    std::printf("\n");
  }
  if (!candidate.notes().empty()) {
    std::printf("notes: %s\n", candidate.notes().c_str());
  }
  if (!candidate.touched_paths().empty()) {
    std::printf("\npatch touches:\n");
    for (const std::string &path : candidate.touched_paths()) {
      std::printf("  %s\n", path.c_str());
    }
  }
  std::printf("\n");
  if (!candidate.file_paths().empty()) {
    std::printf("readable here (files the patch adds):\n");
    for (const std::string &path : candidate.file_paths()) {
      std::printf("  %s\n", path.c_str());
    }
  } else {
    std::printf(
        "readable here: none -- this submission only modifies existing "
        "files. Read them from your own checkout at base_commit.\n");
  }
  if (!candidate.build_error().empty()) {
    std::printf("\nbuild error:\n%s\n", candidate.build_error().c_str());
  }
  return 0;
}

auto CmdEvaluate(const Client &client, const std::string &server,
                 const std::vector<char *> &args) -> int {
  if (args.empty()) {
    std::fprintf(stderr, "evaluate: a candidate id is required\n");
    return kExitUsage;
  }
  const std::string opponent = absl::GetFlag(FLAGS_opponent);
  const int repeats = absl::GetFlag(FLAGS_repeats);
  const int games = absl::GetFlag(FLAGS_games);
  if (repeats > 0 && !opponent.empty()) {
    std::fprintf(stderr,
                 "evaluate: pass --opponent (match problem) or --repeats "
                 "(graded problem), not both\n");
    return kExitUsage;
  }

  proto::EvaluateRequest request;
  request.set_candidate_id(args[0]);
  request.set_cancel_running(absl::GetFlag(FLAGS_cancel_running));
  if (repeats > 0) {
    request.mutable_grade()->set_repeats(repeats);
  } else {
    request.mutable_match()->set_opponent(
        opponent.empty() ? "ladder" : opponent);
    if (games > 0) {
      request.mutable_match()->set_games(games);
    }
  }

  grpc::ClientContext context;
  ConfigureContext(client, /*write=*/true, &context);
  proto::EvaluateResponse response;
  const grpc::Status status =
      client.stub->Evaluate(&context, request, &response);
  if (!status.ok()) {
    return RpcError(status, server);
  }
  std::printf("job %s queued\n", response.job_id().c_str());
  if (!absl::GetFlag(FLAGS_wait)) {
    std::printf("poll: job %s [--wait]\n", response.job_id().c_str());
    return 0;
  }
  return WaitForJob(client, server, response.job_id());
}

void PrintUsage() {
  std::fprintf(
      stderr,
      "usage: arena_cli [--server=host:port] [--token=...] <command> ...\n"
      "\n"
      "  rules                          this server's problem and limits\n"
      "  submit --name=... [--file=f ... | --patch=d] [--wait]\n"
      "                                 submit a candidate, queue its build\n"
      "  job <job_id> [--wait]          build/match status\n"
      "  candidates [--order=best|newest] [--author=a] [--limit=n]\n"
      "                                 everyone, including pending/broken\n"
      "  leaderboard [--limit=n]        current standings\n"
      "  source <candidate_id> [path]   a rival's manifest, or one file\n"
      "  evaluate <candidate_id> [--opponent=o|--repeats=n] [--wait]\n"
      "                                 queue more games or more runs\n");
}

}  // namespace

auto main(int argc, char **argv) -> int {
  const std::vector<char *> positional = absl::ParseCommandLine(argc, argv);
  absl::InitializeLog();
  absl::SetStderrThreshold(absl::LogSeverityAtLeast::kWarning);

  if (positional.size() < 2) {
    PrintUsage();
    return kExitUsage;
  }
  const std::string command = positional[1];
  const std::vector<char *> args(positional.begin() + 2, positional.end());

  std::string server = absl::GetFlag(FLAGS_server);
  if (server.empty()) {
    server = EnvOr("ARENA_SERVER", "localhost:50051");
  }
  std::string token = absl::GetFlag(FLAGS_token);
  if (token.empty()) {
    token = EnvOr("ARENA_TOKEN", "");
  }
  const Client client{proto::Arena::NewStub(grpc::CreateChannel(
                          server, grpc::InsecureChannelCredentials())),
                      token, absl::GetFlag(FLAGS_timeout_s)};

  if (command == "rules") {
    return CmdRules(client, server);
  }
  if (command == "submit") {
    return CmdSubmit(client, server);
  }
  if (command == "job") {
    return CmdJob(client, server, args);
  }
  if (command == "candidates") {
    return CmdCandidates(client, server);
  }
  if (command == "leaderboard") {
    return CmdLeaderboard(client, server);
  }
  if (command == "source") {
    return CmdSource(client, server, args);
  }
  if (command == "evaluate") {
    return CmdEvaluate(client, server, args);
  }
  PrintUsage();
  return kExitUsage;
}
