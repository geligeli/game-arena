// Command-line client for the arena's Arena service.
/*
arena_cli rules
arena_cli submit --wait                          # your directory, bots/<you>/
arena_cli submit --file=strategy.h --wait
arena_cli submit --patch=my.diff
arena_cli job <job_id> [--wait]
arena_cli candidates [--order=newest]
arena_cli leaderboard [--limit=20]
arena_cli source <name> [path]   # pulls their directory in beside yours
arena_cli spar <name> [--games=10]   # and plays yours against it, here
arena_cli spar builtin:greedy        # or against a builtin
*/
//
// The human's counterpart to the MCP server: same RPCs, same compact output,
// drivable from a shell. Submit is the write path and carries the
// --token as x-arena-token metadata; the reads are open unless the problem
// says otherwise (ProblemInfo.source_visibility).
//
// It ships inside a kit as a binary, not as a bazel target: submitting should
// not need a toolchain, and a participant should not have to know what the
// arena's build looks like to enter a tournament. The kit's arena.textproto
// (proto/kit.proto) is where it gets its defaults -- the coordinator's
// address, what a solution is made of, where pulled rivals land. That file is
// the participant's to edit: widening it changes what this tool sends, never
// what the coordinator accepts.

#include <fcntl.h>
#include <google/protobuf/text_format.h>
#include <grpcpp/grpcpp.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "absl/flags/flag.h"
#include "absl/flags/parse.h"
#include "absl/log/globals.h"
#include "absl/log/initialize.h"
#include "game_arena/proto/arena.grpc.pb.h"
#include "game_arena/proto/kit.pb.h"
#include "game_arena/proto/tournament_broker.pb.h"
#include "game_arena/referee/match_tally.h"

ABSL_FLAG(std::string, server, "",
          "Arena address. Default: $ARENA_SERVER, else the kit's "
          "arena.textproto, else localhost:50051");
ABSL_FLAG(std::string, kit, "",
          "The kit directory whose arena.textproto configures this tool. "
          "Default: $ARENA_KIT, else the nearest enclosing directory that "
          "has one");
ABSL_FLAG(std::string, token, "",
          "Bearer token for Submit, sent as the x-arena-token "
          "metadata header (default: $ARENA_TOKEN)");
ABSL_FLAG(int, timeout_s, 120, "Per-RPC timeout in seconds");

ABSL_FLAG(std::string, name, "",
          "submit: display name for the candidate. Default: who your token "
          "says you are");
ABSL_FLAG(std::vector<std::string>, file, {},
          "submit: a source file to submit (repeatable). Read from disk, "
          "flattened to its basename; the server generates the BUILD");
ABSL_FLAG(std::string, patch, "",
          "submit: a unified diff against the problem's tree, read from "
          "disk. Mutually exclusive with --file");
ABSL_FLAG(std::string, entry_header, "",
          "submit: the header defining MakePolicy. Defaults to the kit's "
          "entry_header, else the sole .h/.hpp among the files submitted");
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
          "submit: abort this client's in-flight work and take its "
          "place, instead of being refused for being over quota");
ABSL_FLAG(bool, wait, false,
          "submit/job: poll until the job reaches a terminal state");

ABSL_FLAG(std::string, author, "", "candidates: restrict to this author");
ABSL_FLAG(std::string, order, "best", "candidates: best | newest");
ABSL_FLAG(int, limit, 20, "candidates/leaderboard: max rows (0: default)");

ABSL_FLAG(bool, print, false,
          "source: write the files to stdout instead of into the kit");

ABSL_FLAG(int, games, 10, "spar: games to play");

namespace {

namespace proto = tournament_arena::proto;

constexpr int kPollIntervalS = 2;
constexpr int kExitError = 1;
constexpr int kExitUsage = 2;

std::string EnvOr(const char *name, std::string fallback) {
  if (const char *value = std::getenv(name); value != nullptr) {
    return value;
  }
  return fallback;
}

struct Client {
  std::unique_ptr<proto::Arena::Stub> stub;
  std::string token;
  int timeout_s;
  // The kit this was run from, and its config. Both empty outside a kit,
  // which is a working state: every default the config carries can also be
  // given as a flag.
  std::filesystem::path kit_dir;
  proto::KitConfig kit;
  // Who this kit is: $ARENA_NAME, else the kit's client_id. What the
  // participant's own directory is called, here and at the tournament.
  std::string me;

  // <kit>/<submit_dir>/<name>: where a participant's implementation lives.
  std::filesystem::path DirOf(const std::string &name) const {
    return kit_dir / kit.submit_dir() / name;
  }
};

// The kit this command belongs to: --kit, $ARENA_KIT, or the nearest
// enclosing directory holding an arena.textproto. Empty when there is none.
std::filesystem::path FindKit() {
  if (const std::string flag = absl::GetFlag(FLAGS_kit); !flag.empty()) {
    return flag;
  }
  if (const std::string env = EnvOr("ARENA_KIT", ""); !env.empty()) {
    return env;
  }
  std::error_code ec;
  std::filesystem::path dir = std::filesystem::current_path(ec);
  if (ec) {
    return {};
  }
  for (; !dir.empty(); dir = dir.parent_path()) {
    if (std::filesystem::exists(dir / "arena.textproto")) {
      return dir;
    }
    if (!dir.has_relative_path()) {
      break;  // at the root: stop before parent_path() stops moving
    }
  }
  return {};
}

// The kit's config, or an empty one. A kit whose config does not parse is
// worth saying out loud: the participant edited it, and the fix is theirs.
proto::KitConfig LoadKitConfig(const std::filesystem::path &kit) {
  proto::KitConfig config;
  if (kit.empty()) {
    return config;
  }
  const std::filesystem::path path = kit / "arena.textproto";
  std::ifstream stream(path);
  if (!stream) {
    return config;
  }
  std::ostringstream buffer;
  buffer << stream.rdbuf();
  if (!google::protobuf::TextFormat::ParseFromString(buffer.str(), &config)) {
    std::fprintf(stderr,
                 "WARNING: %s does not parse; falling back to flags and "
                 "environment\n",
                 path.c_str());
    config.Clear();
  }
  return config;
}

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
int RpcError(const grpc::Status &status, const std::string &server) {
  switch (status.error_code()) {
    case grpc::StatusCode::UNAUTHENTICATED:
      std::fprintf(stderr,
                   "ERROR: %s\n"
                   "Your token is ARENA_TOKEN (a kit sets it in arena.env, "
                   "and bakes it into its image). The tournament's operator "
                   "issues it.\n",
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
                   "ERROR: no arena at %s. That address comes from --server, "
                   "else $ARENA_SERVER, else the kit's arena.textproto; the "
                   "tournament has to be running and reachable from here.\n",
                   server.c_str());
      return kExitError;
    case grpc::StatusCode::PERMISSION_DENIED:
      std::fprintf(stderr,
                   "ERROR: %s\n"
                   "That is this tournament's rule, not this tool's: `rules` "
                   "prints it.\n",
                   status.error_message().c_str());
      return kExitError;
    default:
      std::fprintf(stderr, "ERROR: %s\n", status.error_message().c_str());
      return kExitError;
  }
}

const char *StatusName(proto::Candidate::Status status) {
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

const char *JobStateName(proto::Job::State state) {
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
const char *PhaseName(proto::OrderProgress::Phase phase) {
  switch (phase) {
    case proto::OrderProgress::PREPARING:
      return "preparing";
    case proto::OrderProgress::BUILDING:
      return "building";
    case proto::OrderProgress::RUNNING:
      return "running";
    default:
      return "?";
  }
}

bool IsTerminal(proto::Job::State state) {
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
  const std::string state = job.state() == proto::Job::RUNNING
                                ? std::string(JobStateName(job.state())) +
                                      ", " + PhaseName(job.phase())
                                : JobStateName(job.state());
  std::printf("job %s [%s] candidate %s\n", job.job_id().c_str(), state.c_str(),
              job.candidate_id().c_str());
  std::printf("games %d/%d  W/D/L %d/%d/%d  score %.3f\n", job.games_played(),
              job.games_requested(), job.wins(), job.draws(), job.losses(),
              job.elo());
  if (!job.error().empty()) {
    std::printf("\n%s\n", job.error().c_str());
  }
}

// Polls until the job reaches a terminal state, printing state transitions.
// Returns the exit code: 0 on DONE, 1 on FAILED/CANCELLED or an RPC error.
int WaitForJob(const Client &client, const std::string &server,
               const std::string &job_id) {
  proto::Job::State last = proto::Job::QUEUED;
  // Tracked alongside the state so a long build reports preparing, then
  // building, then running, instead of one "running" line for half an hour.
  std::optional<proto::OrderProgress::Phase> last_phase;
  for (;;) {
    proto::GetJobRequest request;
    request.set_job_id(job_id);
    proto::Job job;
    grpc::ClientContext context;
    ConfigureContext(client, /*write=*/false, &context);
    const grpc::Status status = client.stub->GetJob(&context, request, &job);
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

int CmdRules(const Client &client, const std::string &server) {
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
  std::printf("\n");
  if (!problem.description().empty()) {
    std::printf("%s\n\n", problem.description().c_str());
  }
  std::printf("SUBMITTING\n");
  if (!client.me.empty() && !client.kit.submit_dir().empty()) {
    std::printf(
        "  submit                         sends %s/%s/\n"
        "                                 (--file overrides)\n",
        client.kit.submit_dir().c_str(), client.me.c_str());
  }
  if (!problem.files_submit_dir().empty()) {
    std::printf(
        "  files:  submit --file=strategy.h\n"
        "          (placed under %s/<you>/, BUILD generated)\n"
        "  patch:  submit --patch=my.diff\n",
        problem.files_submit_dir().c_str());
  } else {
    std::printf(
        "  patch only: submit --patch=my.diff\n"
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
  std::printf("\nREADING OTHERS\n");
  switch (problem.source_visibility()) {
    case proto::ProblemInfo::SOURCE_OWN:
      std::printf(
          "  your own submissions only: `source <id>` of a rival is "
          "refused\n");
      break;
    case proto::ProblemInfo::SOURCE_NONE:
      std::printf("  nothing: no submission's source is served here\n");
      break;
    default:
      std::printf(
          "  everyone: `source <name>` pulls theirs into %s/<name>/, and\n"
          "  `spar <name>` plays yours against it here\n",
          client.kit.submit_dir().c_str());
      break;
  }
  return 0;
}

// Under `bazel run` the working directory is the runfiles tree, so a relative
// path is taken from the workspace the command was run in, which is where the
// file the user means actually is.
auto FromWorkspace(const std::string &path) -> std::filesystem::path {
  const std::filesystem::path p(path);
  const char *workspace = std::getenv("BUILD_WORKSPACE_DIRECTORY");
  if (p.is_relative() && workspace != nullptr && *workspace != '\0') {
    return std::filesystem::path(workspace) / p;
  }
  return p;
}

bool ReadFile(const std::string &path, std::string *content) {
  std::ifstream stream(FromWorkspace(path), std::ios::binary);
  if (!stream) {
    return false;
  }
  std::ostringstream buffer;
  buffer << stream.rdbuf();
  *content = buffer.str();
  return true;
}

// The sources in |dir|, sorted. Not its BUILD: the coordinator generates the
// one a submission is compiled with.
std::vector<std::string> SourcesIn(const std::filesystem::path &dir) {
  static constexpr std::array<std::string_view, 5> kSources = {
      ".h", ".hpp", ".cc", ".cpp", ".inl"};
  std::vector<std::string> found;
  std::error_code ec;
  for (const auto &item :
       std::filesystem::recursive_directory_iterator(dir, ec)) {
    const std::string extension = item.path().extension().string();
    if (item.is_regular_file() && std::find(kSources.begin(), kSources.end(),
                                            extension) != kSources.end()) {
      found.push_back(item.path().string());
    }
  }
  std::sort(found.begin(), found.end());
  return found;
}

int CmdSubmit(const Client &client, const std::string &server) {
  std::vector<std::string> files = absl::GetFlag(FLAGS_file);
  const std::string patch_path = absl::GetFlag(FLAGS_patch);
  const std::string name =
      absl::GetFlag(FLAGS_name).empty() ? client.me : absl::GetFlag(FLAGS_name);

  // No files and no patch: your directory is your solution. This is the
  // ordinary way to submit from a kit -- `submit` and nothing else.
  if (files.empty() && patch_path.empty() && !client.me.empty()) {
    files = SourcesIn(client.DirOf(client.me));
    std::printf("submitting %zu file(s) from %s:\n", files.size(),
                client.DirOf(client.me).c_str());
    for (const std::string &file : files) {
      std::printf("  %s\n", file.c_str());
    }
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
  // Who a coordinator with no registry takes this to be from; one with a
  // registry goes by the token.
  request.set_author(client.me);
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
      } else if (headers.empty() && files.size() == 1) {
        // A standalone program: the one file is the entry.
        entry_header = std::filesystem::path(files.front()).filename().string();
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
    request.set_entry_header(
        std::filesystem::path(entry_header).filename().string());
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

int CmdJob(const Client &client, const std::string &server,
           const std::vector<char *> &args) {
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
  return IsTerminal(job.state()) && job.state() != proto::Job::DONE ? kExitError
                                                                    : 0;
}

int CmdCandidates(const Client &client, const std::string &server) {
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

int CmdLeaderboard(const Client &client, const std::string &server) {
  proto::LeaderboardRequest request;
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

// One file of a candidate, into the kit under |into| (empty: stdout). Paths
// are repo-relative, which in a kit is where they belong -- but only below
// the candidate's own directory: one that lands anywhere else is refused
// rather than written.
int PullSourceFile(const Client &client, const std::string &server,
                   const std::string &candidate_id, const std::string &path,
                   const std::filesystem::path &into) {
  proto::GetSourceRequest request;
  request.set_candidate_id(candidate_id);
  request.set_path(path);
  grpc::ClientContext context;
  ConfigureContext(client, /*write=*/false, &context);
  proto::SourceFile source;
  const grpc::Status status =
      client.stub->GetSource(&context, request, &source);
  if (!status.ok()) {
    return RpcError(status, server);
  }
  if (into.empty()) {
    std::fwrite(source.content().data(), 1, source.content().size(), stdout);
    return 0;
  }
  const std::filesystem::path target =
      (client.kit_dir / path).lexically_normal();
  const std::string prefix = into.lexically_normal().string() + "/";
  if (target.string().rfind(prefix, 0) != 0) {
    std::fprintf(stderr, "source: refusing path '%s'\n", path.c_str());
    return kExitError;
  }
  std::error_code ec;
  std::filesystem::create_directories(target.parent_path(), ec);
  std::ofstream out(target, std::ios::binary | std::ios::trunc);
  out.write(source.content().data(),
            static_cast<std::streamsize>(source.content().size()));
  if (!out) {
    std::fprintf(stderr, "source: cannot write %s\n", target.c_str());
    return kExitError;
  }
  std::printf("  %s\n", target.c_str());
  return 0;
}

int CmdSource(const Client &client, const std::string &server,
              const std::vector<char *> &args) {
  if (args.empty()) {
    std::fprintf(stderr, "source: a candidate id is required\n");
    return kExitUsage;
  }
  const std::string candidate_id = args[0];
  if (args.size() > 1) {
    return PullSourceFile(client, server, candidate_id, args[1], {});
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
  std::printf("parent %s\n", candidate.parent_id().empty()
                                 ? "-"
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
  if (!candidate.build_error().empty()) {
    std::printf("build error:\n%s\n\n", candidate.build_error().c_str());
  }
  if (candidate.file_paths().empty()) {
    std::printf(
        "no files to read: this submission only modifies existing code. Its "
        "diff is the patch on this manifest; your own kit holds what it "
        "changed.\n");
    return 0;
  }

  // Where it lands: their directory, beside yours. Your own is printed
  // instead, because what is there is what you are working on.
  const bool to_stdout = absl::GetFlag(FLAGS_print) || client.kit_dir.empty() ||
                         client.kit.submit_dir().empty() ||
                         candidate.candidate_id() == client.me;
  if (to_stdout) {
    for (const std::string &path : candidate.file_paths()) {
      std::printf("----- %s -----\n", path.c_str());
      if (const int code = PullSourceFile(client, server,
                                          candidate.candidate_id(), path, {});
          code != 0) {
        return code;
      }
    }
    return 0;
  }

  const std::filesystem::path into = client.DirOf(candidate.candidate_id());
  std::error_code ec;
  std::filesystem::create_directories(into, ec);
  if (ec) {
    std::fprintf(stderr, "source: cannot create %s: %s\n", into.c_str(),
                 ec.message().c_str());
    return kExitError;
  }
  std::printf("pulled into %s:\n", into.c_str());
  for (const std::string &path : candidate.file_paths()) {
    if (const int code = PullSourceFile(client, server,
                                        candidate.candidate_id(), path, into);
        code != 0) {
      return code;
    }
  }
  return 0;
}

extern "C" char **environ;

// Starts |argv| with its output in |log| (empty: this terminal), and with the
// environment minus ARENA_TOKEN: a rival's code has no use for your
// credential. posix_spawn rather than fork, which a process with gRPC's
// threads in it should not do.
pid_t Spawn(const std::vector<std::string> &argv, const std::string &log) {
  std::vector<char *> args;
  for (const std::string &arg : argv) {
    args.push_back(const_cast<char *>(arg.c_str()));
  }
  args.push_back(nullptr);
  std::vector<char *> env;
  for (char **entry = environ; *entry != nullptr; ++entry) {
    if (std::string_view(*entry).rfind("ARENA_TOKEN=", 0) != 0) {
      env.push_back(*entry);
    }
  }
  env.push_back(nullptr);
  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  if (!log.empty()) {
    posix_spawn_file_actions_addopen(&actions, 1, log.c_str(),
                                     O_WRONLY | O_CREAT | O_TRUNC, 0644);
    posix_spawn_file_actions_adddup2(&actions, 1, 2);
  }
  pid_t pid = -1;
  posix_spawnp(&pid, args[0], &actions, nullptr, args.data(), env.data());
  posix_spawn_file_actions_destroy(&actions);
  return pid;
}

int Wait(pid_t pid) {
  int status = 0;
  ::waitpid(pid, &status, 0);
  return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}

// Yours against |name|'s, here: their directory pulled in beside yours, both
// built, and the games refereed the way the tournament's workers do it -- the
// same referee, the same bounds on a game, two bots naming each other.
int CmdSpar(const Client &client, const std::string &server,
            const std::vector<char *> &args) {
  if (args.empty() || client.me.empty() || client.kit.game().empty()) {
    std::fprintf(stderr,
                 "spar <name>: from the kit of a problem played as matches, "
                 "as someone ($ARENA_NAME)\n");
    return kExitUsage;
  }
  const std::string me = client.me;
  const std::string rival = args[0];
  // A builtin plays inside the referee; there is nothing of it to pull or run.
  const bool builtin = rival.rfind("builtin:", 0) == 0;
  // Pulled every time: a name stays, what is under it does not. Someone who
  // is only in this kit -- the starter -- is played as they are.
  if (!builtin && CmdSource(client, server, args) != 0 &&
      !std::filesystem::exists(client.DirOf(rival))) {
    return kExitError;
  }

  std::filesystem::current_path(client.kit_dir);
  const std::string dir = client.kit.submit_dir();
  const std::string bin = client.kit.bot_binary();
  std::vector<std::string> build = {"bazel", "build", "//:match_referee",
                                    "//" + dir + "/" + me + ":" + bin};
  if (!builtin) {
    build.push_back("//" + dir + "/" + rival + ":" + bin);
  }
  if (Wait(Spawn(build, "")) != 0) {
    return kExitError;
  }

  const std::string opponent = builtin ? rival : "player:" + rival;
  char scratch_template[] = "/tmp/spar.XXXXXX";
  const std::string scratch = ::mkdtemp(scratch_template);
  const std::string games = std::to_string(absl::GetFlag(FLAGS_games));
  std::vector<std::string> referee = {"bazel-bin/match_referee",
                                      "--port=0",
                                      "--port_file=" + scratch + "/port",
                                      "--game=" + client.kit.game(),
                                      "--games=" + games,
                                      "--player_a=" + me,
                                      "--player_b=" + opponent,
                                      "--scratch_dir=" + scratch,
                                      "--report=" + scratch + "/match.pb",
                                      "--deadline_s=900"};
  referee.insert(referee.end(), client.kit.referee_flags().begin(),
                 client.kit.referee_flags().end());
  const pid_t refereeing = Spawn(referee, scratch + "/referee.log");
  // The port file appears once the referee is listening.
  while (!std::filesystem::exists(scratch + "/port")) {
    int status = 0;
    if (::waitpid(refereeing, &status, WNOHANG) == refereeing) {
      std::fprintf(stderr, "spar: the referee exited; see %s/referee.log\n",
                   scratch.c_str());
      return kExitError;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  std::string port;
  std::ifstream(scratch + "/port") >> port;

  const auto bot = [&](const std::string &name, const std::string &against) {
    return Spawn({"bazel-bin/" + dir + "/" + name + "/" + bin, "--name=" + name,
                  "--server=localhost:" + port, "--opponent=" + against,
                  "--games=" + games},
                 scratch + "/" + name + ".log");
  };
  const pid_t mine = bot(me, opponent);
  const pid_t theirs = builtin ? -1 : bot(rival, "player:" + me);
  Wait(refereeing);
  Wait(mine);
  if (theirs > 0) {
    Wait(theirs);
  }

  // Counted from player_a's side: yours.
  tournament_broker::proto::MatchReport report;
  std::ifstream in(scratch + "/match.pb", std::ios::binary);
  report.ParseFromIstream(&in);
  const tournament_broker::MatchTally tally =
      tournament_broker::TallyOf(report, me);
  std::printf("\n%s %d   draws %d   %s %d   (%d of %s games; logs in %s)\n",
              me.c_str(), tally.wins, tally.draws, rival.c_str(), tally.losses,
              tally.games, games.c_str(), scratch.c_str());
  return tally.games > 0 ? 0 : kExitError;
}

void PrintUsage() {
  std::fprintf(
      stderr,
      "usage: arena_cli [--server=host:port] [--token=...] <command> ...\n"
      "\n"
      "  rules                          this server's problem and limits\n"
      "  submit [--file=f ... | --patch=d] [--wait]\n"
      "                                 submit, queue its build. With no\n"
      "                                 --file, your directory\n"
      "  job <job_id> [--wait]          build/match status\n"
      "  candidates [--order=best|newest] [--author=a] [--limit=n]\n"
      "                                 everyone, including pending/broken\n"
      "  leaderboard [--limit=n]        current standings\n"
      "  source <name> [path]           pull a participant's directory in\n"
      "                                 beside yours (or one file to stdout)\n"
      "  spar <name> [--games=n]        that, then play yours against it "
      "here\n"
      "  spar builtin:<name>            yours against one of the problem's "
      "builtins\n");
}

}  // namespace

int main(int argc, char **argv) {
  const std::vector<char *> positional = absl::ParseCommandLine(argc, argv);
  absl::InitializeLog();
  absl::SetStderrThreshold(absl::LogSeverityAtLeast::kWarning);

  if (positional.size() < 2) {
    PrintUsage();
    return kExitUsage;
  }
  const std::string command = positional[1];
  const std::vector<char *> args(positional.begin() + 2, positional.end());

  // The kit's config is the last word on every default, after the flag and
  // the environment: a participant who exports ARENA_SERVER means it.
  const std::filesystem::path kit_dir = FindKit();
  proto::KitConfig kit = LoadKitConfig(kit_dir);

  std::string server = absl::GetFlag(FLAGS_server);
  if (server.empty()) {
    server = EnvOr("ARENA_SERVER", "");
  }
  if (server.empty()) {
    server = kit.server();
  }
  if (server.empty()) {
    server = "localhost:50051";
  }
  std::string token = absl::GetFlag(FLAGS_token);
  if (token.empty()) {
    token = EnvOr("ARENA_TOKEN", "");
  }
  const std::string me = EnvOr("ARENA_NAME", kit.client_id());
  const Client client{proto::Arena::NewStub(grpc::CreateChannel(
                          server, grpc::InsecureChannelCredentials())),
                      token,
                      absl::GetFlag(FLAGS_timeout_s),
                      kit_dir,
                      std::move(kit),
                      me};
  // Yours is a directory like everyone's, named after you. The first time, it
  // is a copy of the starter's.
  if (!me.empty() && !client.kit.starter_dir().empty() &&
      !std::filesystem::exists(client.DirOf(me))) {
    std::error_code ec;
    std::filesystem::copy(kit_dir / client.kit.starter_dir(), client.DirOf(me),
                          std::filesystem::copy_options::recursive, ec);
    std::printf("%s is yours, started from %s\n\n", client.DirOf(me).c_str(),
                client.kit.starter_dir().c_str());
  }

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
  if (command == "spar") {
    return CmdSpar(client, server, args);
  }
  PrintUsage();
  return kExitUsage;
}
