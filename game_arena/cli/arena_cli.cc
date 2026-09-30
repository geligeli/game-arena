// The participant's client, for a shell and (as `mcp`) for an agent.

#include <fcntl.h>
#include <google/protobuf/text_format.h>
#include <grpcpp/grpcpp.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
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
#include "absl/strings/str_join.h"
#include "absl/strings/str_split.h"
#include "game_arena/cli/mcp.h"
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
using Stub = proto::Arena::Stub;

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
  std::string server;
  std::string token;
  int timeout_s;
  // Both empty outside a kit, which works: every default is also a flag.
  std::filesystem::path kit_dir;
  proto::KitConfig kit;
  std::string me;

  std::filesystem::path DirOf(const std::string &name) const {
    return kit_dir / kit.submit_dir() / name;
  }
};

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

void ConfigureContext(const Client &client, grpc::ClientContext *context) {
  context->set_deadline(std::chrono::system_clock::now() +
                        std::chrono::seconds(client.timeout_s));
  if (!client.token.empty()) {
    context->AddMetadata("x-arena-token", client.token);
  }
}

void RpcError(const grpc::Status &status, const std::string &server) {
  switch (status.error_code()) {
    case grpc::StatusCode::UNAUTHENTICATED:
      std::fprintf(stderr,
                   "ERROR: %s\n"
                   "Your token is ARENA_TOKEN (a kit sets it in arena.env, "
                   "and bakes it into its image). The tournament's operator "
                   "issues it.\n",
                   status.error_message().c_str());
      return;
    case grpc::StatusCode::RESOURCE_EXHAUSTED:
      std::fprintf(stderr,
                   "ERROR: over quota. %s\n"
                   "Poll `job` until the running one finishes, or pass "
                   "--cancel_running to replace it.\n",
                   status.error_message().c_str());
      return;
    case grpc::StatusCode::UNAVAILABLE:
      std::fprintf(stderr,
                   "ERROR: no arena at %s. That address comes from --server, "
                   "else $ARENA_SERVER, else the kit's arena.textproto; the "
                   "tournament has to be running and reachable from here.\n",
                   server.c_str());
      return;
    case grpc::StatusCode::PERMISSION_DENIED:
      std::fprintf(stderr,
                   "ERROR: %s\n"
                   "That is this tournament's rule, not this tool's: `rules` "
                   "prints it.\n",
                   status.error_message().c_str());
      return;
    default:
      std::fprintf(stderr, "ERROR: %s\n", status.error_message().c_str());
  }
}

// False, with the error printed, when the call fails.
template <typename Method, typename Request, typename Response>
bool Call(const Client &client, Method method, const Request &request,
          Response *response) {
  grpc::ClientContext context;
  ConfigureContext(client, &context);
  const grpc::Status status =
      (client.stub.get()->*method)(&context, request, response);
  if (!status.ok()) {
    RpcError(status, client.server);
  }
  return status.ok();
}

// As printed: BUILD_FAILED is "build-failed", and a value with no name "?".
template <typename Enum>
std::string NameOf(Enum value) {
  const auto *named =
      google::protobuf::GetEnumDescriptor<Enum>()->FindValueByNumber(value);
  std::string name = named == nullptr ? "?" : std::string(named->name());
  for (char &c : name) {
    c = c == '_' ? '-' : static_cast<char>(std::tolower(c));
  }
  return name;
}

bool IsTerminal(proto::Job::State state) {
  return state == proto::Job::DONE || state == proto::Job::FAILED ||
         state == proto::Job::CANCELLED;
}

// Seats per game, from the kit: a kit from before it has two.
int SeatsOf(const Client &client) {
  return client.kit.players() > 0 ? client.kit.players() : 2;
}

void PrintStandingHeader(const std::string &score_label, bool graded,
                         int seats) {
  if (graded) {
    std::printf("%-28s %9s %5s %-12s %-12s %-12s %s\n", "candidate_id",
                score_label.empty() ? "score" : score_label.c_str(), "runs",
                "machine", "status", "author", "name");
  } else {
    std::printf("%-28s %9s %11s %5s %-12s %-12s %s\n", "candidate_id",
                score_label.empty() ? "score" : score_label.c_str(),
                seats == 2 ? "W/D/L" : "places", "games", "status", "author",
                "name");
  }
}

void PrintStandingRow(const proto::CandidateStanding &standing, bool graded,
                      int seats) {
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
    if (seats == 2) {
      std::printf("%3d/%3d/%3d %4dg ", standing.wins(), standing.draws(),
                  standing.losses(), played);
    } else {
      std::printf("%11s %4dg ", absl::StrJoin(standing.finishes(), "/").c_str(),
                  played);
    }
  }
  std::printf("%-12s %-12s %s", NameOf(candidate.status()).c_str(),
              candidate.author().empty() ? "-" : candidate.author().c_str(),
              candidate.display_name().c_str());
  if (!candidate.parent_id().empty()) {
    std::printf("  <- %s", candidate.parent_id().c_str());
  }
  std::printf("\n");
}

void PrintJob(const proto::Job &job, int seats) {
  std::string state = NameOf(job.state());
  if (job.state() == proto::Job::RUNNING) {
    state += ", " + NameOf(job.phase());
  }
  std::printf("job %s [%s] candidate %s\n", job.job_id().c_str(), state.c_str(),
              job.candidate_id().c_str());
  std::printf("games %d/%d  %s  score %.3f\n", job.games_played(),
              job.games_requested(),
              tournament_broker::RecordText(seats, job.wins(), job.draws(),
                                            job.losses(), job.finishes())
                  .c_str(),
              job.elo());
  if (!job.error().empty()) {
    std::printf("\n%s\n", job.error().c_str());
  }
}

// Polls while |wait|. 1 once FAILED/CANCELLED, or on an RPC error.
int WaitForJob(const Client &client, const std::string &job_id, bool wait) {
  proto::Job::State last = proto::Job::QUEUED;
  std::optional<proto::OrderProgress::Phase> last_phase;
  for (;;) {
    proto::GetJobRequest request;
    request.set_job_id(job_id);
    proto::Job job;
    if (!Call(client, &Stub::GetJob, request, &job)) {
      return kExitError;
    }
    if (job.state() != last || last_phase != job.phase()) {
      PrintJob(job, SeatsOf(client));
      last = job.state();
      last_phase = job.phase();
    }
    if (IsTerminal(job.state())) {
      return job.state() == proto::Job::DONE ? 0 : kExitError;
    }
    if (!wait) {
      return 0;
    }
    std::this_thread::sleep_for(std::chrono::seconds(kPollIntervalS));
  }
}

int CmdRules(const Client &client) {
  proto::ProblemInfo problem;
  if (!Call(client, &Stub::GetProblem, proto::GetProblemRequest(), &problem)) {
    return kExitError;
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

// Under `bazel run` the cwd is the runfiles tree; the user means their own.
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

// Not BUILD: the coordinator generates the one a submission is built with.
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

int CmdSubmit(const Client &client) {
  std::vector<std::string> files = absl::GetFlag(FLAGS_file);
  const std::string patch_path = absl::GetFlag(FLAGS_patch);
  const std::string name =
      absl::GetFlag(FLAGS_name).empty() ? client.me : absl::GetFlag(FLAGS_name);

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
  // Read only by a coordinator with no registry; one with goes by the token.
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
    // Basename only: a submission is its own directory; a path invites "..".
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

  proto::SubmitResponse response;
  if (!Call(client, &Stub::Submit, request, &response)) {
    return kExitError;
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
  return WaitForJob(client, response.job_id(), true);
}

int CmdJob(const Client &client, const std::vector<char *> &args) {
  if (args.empty()) {
    std::fprintf(stderr, "job: a job id is required\n");
    return kExitUsage;
  }
  return WaitForJob(client, args[0], absl::GetFlag(FLAGS_wait));
}

int CmdCandidates(const Client &client) {
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

  proto::ListCandidatesResponse response;
  if (!Call(client, &Stub::ListCandidates, request, &response)) {
    return kExitError;
  }
  if (response.candidates().empty()) {
    std::printf("no candidates yet\n");
    return 0;
  }
  // No score_label here: a row with metrics is a graded one.
  const bool graded =
      std::any_of(response.candidates().begin(), response.candidates().end(),
                  [](const proto::CandidateStanding &standing) {
                    return !standing.metrics().empty();
                  });
  PrintStandingHeader("score", graded, SeatsOf(client));
  for (const proto::CandidateStanding &standing : response.candidates()) {
    PrintStandingRow(standing, graded, SeatsOf(client));
  }
  return 0;
}

int CmdLeaderboard(const Client &client) {
  proto::LeaderboardRequest request;
  request.set_limit(absl::GetFlag(FLAGS_limit));
  proto::LeaderboardResponse response;
  if (!Call(client, &Stub::Leaderboard, request, &response)) {
    return kExitError;
  }
  if (response.rows().empty()) {
    std::printf("nothing has been scored yet\n");
    return 0;
  }
  const bool graded = response.graded();
  PrintStandingHeader(response.score_label(), graded, SeatsOf(client));
  for (const proto::CandidateStanding &standing : response.rows()) {
    PrintStandingRow(standing, graded, SeatsOf(client));
  }
  return 0;
}

// Into |into| (empty: stdout); a path that would land outside it is refused.
int PullSourceFile(const Client &client, const std::string &candidate_id,
                   const std::string &path, const std::filesystem::path &into) {
  proto::GetSourceRequest request;
  request.set_candidate_id(candidate_id);
  request.set_path(path);
  proto::SourceFile source;
  if (!Call(client, &Stub::GetSource, request, &source)) {
    return kExitError;
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

// Nothing to pull is not an error: false, and the caller uses the starter.
bool Restore(const Client &client) {
  proto::GetCandidateRequest request;
  request.set_candidate_id(client.me);
  grpc::ClientContext context;
  ConfigureContext(client, &context);
  proto::Candidate candidate;
  if (!client.stub->GetCandidate(&context, request, &candidate).ok() ||
      candidate.file_paths().empty()) {
    return false;
  }
  for (const std::string &path : candidate.file_paths()) {
    if (PullSourceFile(client, client.me, path, client.DirOf(client.me)) != 0) {
      return false;
    }
  }
  return true;
}

int CmdSource(const Client &client, const std::vector<char *> &args) {
  if (args.empty()) {
    std::fprintf(stderr, "source: a candidate id is required\n");
    return kExitUsage;
  }
  const std::string candidate_id = args[0];
  if (args.size() > 1) {
    return PullSourceFile(client, candidate_id, args[1], {});
  }

  proto::GetCandidateRequest request;
  request.set_candidate_id(candidate_id);
  proto::Candidate candidate;
  if (!Call(client, &Stub::GetCandidate, request, &candidate)) {
    return kExitError;
  }
  std::printf("%s  \"%s\"\n", candidate.candidate_id().c_str(),
              candidate.display_name().c_str());
  std::printf("author %s  game %s  status %s\n",
              candidate.author().empty() ? "-" : candidate.author().c_str(),
              candidate.game().empty() ? "-" : candidate.game().c_str(),
              NameOf(candidate.status()).c_str());
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

  // Your own goes to stdout: pulling it would overwrite your work in progress.
  const bool to_stdout = absl::GetFlag(FLAGS_print) || client.kit_dir.empty() ||
                         client.kit.submit_dir().empty() ||
                         candidate.candidate_id() == client.me;
  const std::filesystem::path into =
      to_stdout ? std::filesystem::path()
                : client.DirOf(candidate.candidate_id());
  if (!to_stdout) {
    std::error_code ec;
    std::filesystem::create_directories(into, ec);
    if (ec) {
      std::fprintf(stderr, "source: cannot create %s: %s\n", into.c_str(),
                   ec.message().c_str());
      return kExitError;
    }
    std::printf("pulled into %s:\n", into.c_str());
  }
  for (const std::string &path : candidate.file_paths()) {
    if (to_stdout) {
      std::printf("----- %s -----\n", path.c_str());
    }
    if (const int code =
            PullSourceFile(client, candidate.candidate_id(), path, into);
        code != 0) {
      return code;
    }
  }
  return 0;
}

extern "C" char **environ;

// No ARENA_TOKEN unless |with_token|: a rival's code has no use for yours.
// posix_spawn, not fork: this process has gRPC's threads in it.
pid_t Spawn(const std::vector<std::string> &argv, const std::string &log,
            bool with_token = false) {
  std::vector<char *> args;
  for (const std::string &arg : argv) {
    args.push_back(const_cast<char *>(arg.c_str()));
  }
  args.push_back(nullptr);
  std::vector<char *> env;
  for (char **entry = environ; *entry != nullptr; ++entry) {
    if (with_token || std::string_view(*entry).rfind("ARENA_TOKEN=", 0) != 0) {
      env.push_back(*entry);
    }
  }
  env.push_back(nullptr);
  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  if (!log.empty()) {
    posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0);
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

int CmdSpar(const Client &client, const std::vector<char *> &args) {
  if (args.empty() || client.me.empty() || client.kit.game().empty()) {
    std::fprintf(stderr,
                 "spar <name>: from the kit of a problem played as matches, "
                 "as someone ($ARENA_NAME)\n");
    return kExitUsage;
  }
  const std::string me = client.me;
  const int seats = SeatsOf(client);
  std::vector<std::string> rivals;
  for (const char *arg : args) {
    for (const absl::string_view rival :
         absl::StrSplit(arg, ',', absl::SkipEmpty())) {
      rivals.emplace_back(rival);
    }
  }
  // A builtin plays inside the referee, so it can fill every seat left; a
  // player cannot take two.
  while (!rivals.empty() && static_cast<int>(rivals.size()) + 1 < seats &&
         rivals.back().starts_with("builtin:")) {
    rivals.push_back(rivals.back());
  }
  if (static_cast<int>(rivals.size()) + 1 != seats) {
    std::fprintf(stderr,
                 "spar: a game seats %d, so name %d rival(s), or end with a "
                 "builtin to fill the rest\n",
                 seats, seats - 1);
    return kExitUsage;
  }
  std::vector<std::string> players;  // the rivals with code, pulled and built
  std::vector<std::string> specs;
  for (std::string &rival : rivals) {
    const bool builtin = rival.starts_with("builtin:");
    specs.push_back(builtin ? rival : "player:" + rival);
    if (builtin || std::ranges::contains(players, rival)) {
      continue;
    }
    // Pulled every time; one only in this kit (the starter) is played as it
    // is.
    if (CmdSource(client, {rival.data()}) != 0 &&
        !std::filesystem::exists(client.DirOf(rival))) {
      return kExitError;
    }
    players.push_back(rival);
  }

  std::filesystem::current_path(client.kit_dir);
  const std::string dir = client.kit.submit_dir();
  const std::string bin = client.kit.bot_binary();
  std::vector<std::string> build = {"bazel", "build", "//:match_referee",
                                    "//" + dir + "/" + me + ":" + bin};
  for (const std::string &player : players) {
    build.push_back("//" + dir + "/" + player + ":" + bin);
  }
  if (Wait(Spawn(build, "")) != 0) {
    return kExitError;
  }

  char scratch_template[] = "/tmp/spar.XXXXXX";
  const std::string scratch = ::mkdtemp(scratch_template);
  const std::string games = std::to_string(absl::GetFlag(FLAGS_games));
  std::vector<std::string> referee = {"bazel-bin/match_referee",
                                      "--port=0",
                                      "--port_file=" + scratch + "/port",
                                      "--game=" + client.kit.game(),
                                      "--games=" + games,
                                      "--player_a=" + me,
                                      "--player_b=" + absl::StrJoin(specs, ","),
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
  const pid_t mine = bot(me, absl::StrJoin(specs, ","));
  std::vector<pid_t> theirs;
  for (const std::string &player : players) {
    // Every seat but its own, as the referee seats them.
    std::vector<std::string> others = {"player:" + me};
    std::ranges::copy_if(
        specs, std::back_inserter(others),
        [&](const std::string &spec) { return spec != "player:" + player; });
    theirs.push_back(bot(player, absl::StrJoin(others, ",")));
  }
  Wait(refereeing);
  Wait(mine);
  for (const pid_t pid : theirs) {
    Wait(pid);
  }

  // Counted from player_a's side: yours.
  tournament_broker::proto::MatchReport report;
  std::ifstream in(scratch + "/match.pb", std::ios::binary);
  report.ParseFromIstream(&in);
  const tournament_broker::MatchTally tally =
      tournament_broker::TallyOf(report, me);
  if (seats == 2) {
    std::printf("\n%s %d   draws %d   %s %d   (%d of %s games; logs in %s)\n",
                me.c_str(), tally.wins, tally.draws, rivals[0].c_str(),
                tally.losses, tally.games, games.c_str(), scratch.c_str());
  } else {
    std::printf("\n%s %s   (%d of %s games; logs in %s)\n", me.c_str(),
                tournament_broker::RecordText(seats, tally.wins, tally.draws,
                                              tally.losses, tally.finishes)
                    .c_str(),
                tally.games, games.c_str(), scratch.c_str());
  }
  return tally.games > 0 ? 0 : kExitError;
}

// Each tool call reruns this binary with what this one resolved, in the kit.
int CmdMcp(const Client &client) {
  ::setenv("ARENA_SERVER", client.server.c_str(), 1);
  ::setenv("ARENA_TOKEN", client.token.c_str(), 1);
  ::setenv("ARENA_NAME", client.me.c_str(), 1);
  if (!client.kit_dir.empty()) {
    const std::filesystem::path kit = std::filesystem::absolute(client.kit_dir);
    ::setenv("ARENA_KIT", kit.c_str(), 1);
    std::filesystem::current_path(kit);
  }
  arena_cli::ServeMcp(
      std::cin, std::cout, [&](const std::vector<std::string> &argv) {
        char log[] = "/tmp/arena_mcp.XXXXXX";
        ::close(::mkstemp(log));
        // Still this binary if `play` has since replaced the file.
        std::vector<std::string> command = {"/proc/self/exe"};
        command.insert(command.end(), argv.begin(), argv.end());
        const int code = Wait(Spawn(command, log, /*with_token=*/true));
        std::string output;
        ReadFile(log, &output);
        std::filesystem::remove(log);
        return std::pair{code, output};
      });
  return 0;
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
      "builtins\n"
      "  spar <name> <name2> ...        with more seats, a rival for each; a\n"
      "                                 last builtin fills the seats left\n"
      "  mcp                            all of these as MCP tools, on stdio\n"
      "  init                           make your directory: the starter, or\n"
      "                                 with ARENA_RESTORE=1 your last "
      "submission\n");
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
                      server,
                      token,
                      absl::GetFlag(FLAGS_timeout_s),
                      kit_dir,
                      std::move(kit),
                      me};
  // Before anything is printed: stdout is the protocol's.
  if (command == "mcp") {
    return CmdMcp(client);
  }
  if (!me.empty() && !client.kit.starter_dir().empty() &&
      !std::filesystem::exists(client.DirOf(me))) {
    if (!EnvOr("ARENA_RESTORE", "").empty() && Restore(client)) {
      std::printf("%s is yours, restored from your last submission\n\n",
                  client.DirOf(me).c_str());
    } else {
      std::error_code ec;
      std::filesystem::copy(kit_dir / client.kit.starter_dir(),
                            client.DirOf(me),
                            std::filesystem::copy_options::recursive, ec);
      std::printf("%s is yours, started from %s\n\n", client.DirOf(me).c_str(),
                  client.kit.starter_dir().c_str());
    }
  }
  // What a kit image runs as it starts.
  if (command == "init") {
    return 0;
  }

  const std::map<std::string, std::function<int()>> commands = {
      {"rules", [&] { return CmdRules(client); }},
      {"submit", [&] { return CmdSubmit(client); }},
      {"job", [&] { return CmdJob(client, args); }},
      {"candidates", [&] { return CmdCandidates(client); }},
      {"leaderboard", [&] { return CmdLeaderboard(client); }},
      {"source", [&] { return CmdSource(client, args); }},
      {"spar", [&] { return CmdSpar(client, args); }},
  };
  if (const auto it = commands.find(command); it != commands.end()) {
    return it->second();
  }
  PrintUsage();
  return kExitUsage;
}
