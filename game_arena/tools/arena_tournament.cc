// Standing a tournament up from a problem repository, and handing participants
// a kit to enter it.
/*
bazel run //:tournament -- [--no_container] [--workers=2]
bazel run //:kit -- --out=/srv/kits/alice --server=arena:50051 --mint=alice
bazel run //:sandbox_image -- [--push]
bazel test //:config_test

Without the macro, from a problem repo:
bazel run @game_arena//game_arena/tools:arena_tournament -- \
    up --problem_config=problem.textproto --no_container
*/
//
// Four subcommands, one problem config:
//
//   up      a coordinator and N local workers on this checkout. The committed
//           config stays the truth; --no_container derives a loudly-labelled
//           copy with no image, for a host without docker.
//   kit     a participant's workspace: the files the problem names, the
//           arena's CLI and MCP server reachable through @game_arena, a README
//           from the config, and a freshly minted token.
//   check   the config parses and is consistent with the tree. What
//           :config_test runs.
//   image   the problem's sandbox image: toolchain + vendored deps, so builds
//           run with no network.
//
// The tool knows what a problem *config* is, never what a problem is made of.
// Every label, file and name here arrives from the caller.

#include <arpa/inet.h>
#include <google/protobuf/text_format.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <regex>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

#include "absl/flags/flag.h"
#include "absl/flags/parse.h"
#include "absl/log/globals.h"
#include "absl/log/initialize.h"
#include "absl/log/log.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_join.h"
#include "absl/strings/str_split.h"
#include "absl/strings/strip.h"
#include "game_arena/common/process/process.h"
#include "game_arena/server/client_registry.h"
#include "game_arena/server/problem_config.h"
#include "rules_cc/cc/runfiles/runfiles.h"

ABSL_FLAG(std::string, problem_config, "",
          "The problem's .textproto (required). Relative to the workspace "
          "root under `bazel run`, else to the current directory");

// up
ABSL_FLAG(std::string, data_dir, "",
          "up: where the coordinator keeps submissions, ratings and the "
          "workers' checkouts. Default: $ARENA_STATE_DIR/<problem_id>, and "
          "$ARENA_STATE_DIR defaults to ~/.arena. Kept out of the repo so "
          "`bazel test //...` there never descends into a worker's clone");
ABSL_FLAG(int, grpc_port, 50051, "up: the Arena and SandboxFleet port");
ABSL_FLAG(int, http_port, 8090, "up: the leaderboard port");
ABSL_FLAG(int, workers, 1, "up: local sandbox workers to start");
ABSL_FLAG(bool, no_container, false,
          "up: run with sandbox.image cleared and require_container off, so "
          "the workers use the process engine. Submitted code then runs "
          "unsandboxed as you. For a dev loop on a host without docker; "
          "never for anything whose numbers are compared");
ABSL_FLAG(std::string, clients, "",
          "up/kit: the client registry. up passes it to the coordinator; kit "
          "--mint appends to it. Default: <data_dir>/clients.textproto, used "
          "by up only if it exists");

// kit
ABSL_FLAG(std::string, out, "",
          "kit: directory to write the kit into (created). Default: "
          "<data_dir>/kits/<client_id>");
ABSL_FLAG(std::string, server, "localhost:50051",
          "kit: the arena address baked into the kit");
ABSL_FLAG(std::string, http, "localhost:8090",
          "kit: the leaderboard address baked into the kit");
ABSL_FLAG(std::string, mint, "",
          "kit: mint a token for this client_id, append the client to "
          "--clients, and bake the token into the kit");
ABSL_FLAG(std::string, token, "",
          "kit: bake this existing token in instead of minting one");
ABSL_FLAG(std::string, registry, "",
          "kit: label of the problem's GameRegistry() library, for a local "
          "broker in the kit. Supplied by the arena_problem macro");
ABSL_FLAG(std::string, arena_override, "",
          "kit: write a .bazelrc.local pointing @game_arena at this local "
          "checkout, for a participant on the same host");
ABSL_FLAG(bool, check, false,
          "kit: run `bazel build //...` in the kit afterwards");
ABSL_FLAG(bool, force, false, "kit: write into a non-empty --out");

// image
ABSL_FLAG(std::string, tag, "",
          "image: tag to build. Default: the config's sandbox.image");
ABSL_FLAG(bool, push, false, "image: docker push the result");
ABSL_FLAG(std::string, bazel_version, "",
          "image: bazel release to install. Default: the repo's "
          ".bazelversion when it names a release, else the Dockerfile's");
ABSL_FLAG(std::string, docker, "docker", "image: the docker binary");

namespace {

namespace proto = tournament_arena::proto;
using rules_cc::cc::runfiles::Runfiles;

volatile std::sig_atomic_t g_stop_requested = 0;

extern "C" void OnStopSignal(int /*signum*/) { g_stop_requested = 1; }

auto EnvOr(const char *name, std::string fallback) -> std::string {
  const char *value = std::getenv(name);
  return value != nullptr && *value != '\0' ? std::string(value) : fallback;
}

// Under `bazel run`, the checkout the target was run from; otherwise the
// current directory.
auto WorkspaceRoot() -> std::filesystem::path {
  const std::string root = EnvOr("BUILD_WORKSPACE_DIRECTORY", "");
  return root.empty() ? std::filesystem::current_path()
                      : std::filesystem::path(root);
}

auto Resolve(const std::string &path) -> std::filesystem::path {
  const std::filesystem::path p(path);
  return p.is_absolute() ? p : WorkspaceRoot() / p;
}

auto StateDir(std::string_view problem_id) -> std::filesystem::path {
  const std::filesystem::path base =
      EnvOr("ARENA_STATE_DIR", EnvOr("HOME", "/tmp") + "/.arena");
  return base / std::string(problem_id);
}

auto ReadFile(const std::filesystem::path &path) -> std::optional<std::string> {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return std::nullopt;
  }
  return std::string((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());
}

auto WriteFile(const std::filesystem::path &path,
               std::string_view text) -> bool {
  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out << text;
  return static_cast<bool>(out);
}

// Runs |executable| with the caller's stdout/stderr and waits. -1 when it
// could not be started.
auto RunInherit(const std::string &executable,
                const std::vector<std::string> &arguments,
                const std::filesystem::path &cwd) -> int {
  process::ChildOptions options;
  options.cwd = cwd;
  auto child = process::Child::Start(executable, arguments, options);
  if (!child) {
    return -1;
  }
  while (!child->Poll()) {
    if (g_stop_requested) {
      return child->Stop(std::chrono::seconds(5));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  return *child->Poll();
}

// Runs a command and returns its stdout, or nullopt on failure to start or a
// non-zero exit.
auto Capture(const std::string &executable,
             const std::vector<std::string> &arguments,
             const std::filesystem::path &cwd) -> std::optional<std::string> {
  const std::filesystem::path out =
      std::filesystem::temp_directory_path() /
      absl::StrCat("arena_tournament_", ::getpid(), "_",
                   std::chrono::steady_clock::now().time_since_epoch().count());
  process::RunOptions options;
  options.cwd = cwd;
  options.stdout_path = out;
  options.timeout = std::chrono::seconds(60);
  const process::RunResult result =
      process::RunCommand(executable, arguments, options);
  std::optional<std::string> text = ReadFile(out);
  std::filesystem::remove(out);
  if (!result.started || result.exit_code != 0) {
    return std::nullopt;
  }
  return text;
}

auto WaitForPort(int port, std::chrono::seconds timeout) -> bool {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline && !g_stop_requested) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
      return false;
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    const bool open =
        ::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == 0;
    ::close(fd);
    if (open) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }
  return false;
}

// A local path for repo.url, or nullopt when it is a URL.
auto LocalRepoDir(const proto::ProblemConfig &config)
    -> std::optional<std::filesystem::path> {
  const std::string &url = config.repo().url();
  if (url.find("://") != std::string::npos) {
    return std::nullopt;
  }
  const std::size_t colon = url.find(':');
  if (colon != std::string::npos &&
      (url.find('/') == std::string::npos || colon < url.find('/'))) {
    return std::nullopt;
  }
  return std::filesystem::path(url);
}

auto LoadConfig(std::filesystem::path *config_path)
    -> std::optional<proto::ProblemConfig> {
  const std::string flag = absl::GetFlag(FLAGS_problem_config);
  if (flag.empty()) {
    LOG(ERROR) << "--problem_config is required";
    return std::nullopt;
  }
  *config_path = Resolve(flag);
  std::string error;
  auto config = tournament_arena::LoadProblemConfig(*config_path, &error);
  if (!config) {
    LOG(ERROR) << error;
  }
  return config;
}

// ---------------------------------------------------------------------------
// Runfiles
// ---------------------------------------------------------------------------

class ArenaRunfiles {
 public:
  explicit ArenaRunfiles(const char *argv0) {
    std::string error;
    runfiles_.reset(Runfiles::Create(argv0, BAZEL_CURRENT_REPOSITORY, &error));
    if (!runfiles_) {
      LOG(WARNING) << "runfiles unavailable: " << error;
    }
  }

  // A file of the game_arena module, by workspace-relative path.
  auto Locate(const std::string &path) const -> std::filesystem::path {
    std::vector<std::string> candidates;
    if (runfiles_) {
      candidates.push_back(runfiles_->Rlocation("game_arena/" + path));
      candidates.push_back(runfiles_->Rlocation("_main/" + path));
    }
    const std::string dir = EnvOr("RUNFILES_DIR", "");
    if (!dir.empty()) {
      // Bzlmod's canonical name for a module dep, and for the main repo.
      candidates.push_back(dir + "/game_arena+/" + path);
      candidates.push_back(dir + "/_main/" + path);
    }
    for (const std::string &candidate : candidates) {
      if (!candidate.empty() && std::filesystem::exists(candidate)) {
        return std::filesystem::absolute(candidate);
      }
    }
    LOG(ERROR) << "cannot find " << path
               << " in runfiles; is this running under bazel run?";
    return {};
  }

 private:
  std::unique_ptr<Runfiles> runfiles_;
};

// ---------------------------------------------------------------------------
// check
// ---------------------------------------------------------------------------

auto CheckConfig(const proto::ProblemConfig &config,
                 std::string *error) -> bool {
  const std::string &submit_dir = config.submission().files_submit_dir();
  if (!submit_dir.empty()) {
    bool names_submission = false;
    for (const std::string &target : config.build().targets()) {
      names_submission |= target.find("{submission_id}") != std::string::npos;
    }
    if (!names_submission) {
      *error = absl::StrCat(
          "submission.files_submit_dir is \"", submit_dir,
          "\" but no build.targets entry names {submission_id}, so a "
          "structured submission would never be built");
      return false;
    }
    // Only checkable against a real tree: under `bazel test` the config is a
    // lone data file and repo.url resolves into the runfiles.
    const auto repo = LocalRepoDir(config);
    if (repo && std::filesystem::exists(*repo / "MODULE.bazel") &&
        !std::filesystem::is_directory(*repo / submit_dir)) {
      *error = absl::StrCat("submission.files_submit_dir \"", submit_dir,
                            "\" does not exist under ", repo->string());
      return false;
    }
  }
  return true;
}

auto RunCheck() -> int {
  std::filesystem::path config_path;
  const auto config = LoadConfig(&config_path);
  if (!config) {
    return 1;
  }
  std::string error;
  if (!CheckConfig(*config, &error)) {
    LOG(ERROR) << config_path.string() << ": " << error;
    return 1;
  }
  std::printf("OK: %s (%s, %s)\n", config_path.c_str(),
              config->problem_id().c_str(),
              config->has_match() ? "match" : "graded");
  return 0;
}

// ---------------------------------------------------------------------------
// up
// ---------------------------------------------------------------------------

auto RunUp(const ArenaRunfiles &runfiles) -> int {
  std::filesystem::path config_path;
  auto config = LoadConfig(&config_path);
  if (!config) {
    return 1;
  }
  std::string error;
  if (!CheckConfig(*config, &error)) {
    LOG(ERROR) << config_path.string() << ": " << error;
    return 1;
  }

  const std::filesystem::path server_bin =
      runfiles.Locate("game_arena/server/problem_server");
  const std::filesystem::path worker_bin =
      runfiles.Locate("game_arena/sandbox/worker/sandbox_worker");
  if (server_bin.empty() || worker_bin.empty()) {
    return 1;
  }

  const std::filesystem::path data_dir =
      absl::GetFlag(FLAGS_data_dir).empty()
          ? StateDir(config->problem_id())
          : Resolve(absl::GetFlag(FLAGS_data_dir));
  std::error_code ec;
  std::filesystem::create_directories(data_dir / "work", ec);
  if (ec) {
    LOG(ERROR) << "cannot create " << data_dir << ": " << ec.message();
    return 1;
  }

  // The effective config: what actually runs, and what GetProblem serves.
  if (absl::GetFlag(FLAGS_no_container)) {
    config->mutable_sandbox()->clear_image();
    config->mutable_sandbox()->set_require_container(false);
    LOG(WARNING) << "--no_container: sandbox.image cleared. Submitted code "
                    "is built and run by the process engine, as "
                 << EnvOr("USER", "this user")
                 << ", with resource limits but no isolation. Dev only";
  } else if (config->sandbox().image().empty()) {
    LOG(WARNING) << "sandbox.image is empty: orders run on the process "
                    "engine, unsandboxed";
  }
  std::string effective_text;
  google::protobuf::TextFormat::PrintToString(*config, &effective_text);
  const std::filesystem::path effective =
      data_dir / "problem.effective.textproto";
  if (!WriteFile(effective,
                 absl::StrCat("# Written by arena_tournament up from ",
                              config_path.string(),
                              ". Do not edit; edit the source and restart.\n",
                              effective_text))) {
    LOG(ERROR) << "cannot write " << effective;
    return 1;
  }

  // Workers clone repo.url at base_commit: uncommitted edits stay behind.
  if (const auto repo = LocalRepoDir(*config)) {
    const auto status = Capture("git", {"status", "--porcelain"}, *repo);
    if (status && !status->empty()) {
      LOG(WARNING) << "the tree at " << repo->string()
                   << " has uncommitted changes; workers build the committed "
                      "tree and will not see them";
    }
  }

  std::filesystem::path clients = absl::GetFlag(FLAGS_clients).empty()
                                      ? data_dir / "clients.textproto"
                                      : Resolve(absl::GetFlag(FLAGS_clients));
  const bool gated = std::filesystem::exists(clients);

  const int grpc_port = absl::GetFlag(FLAGS_grpc_port);
  const int http_port = absl::GetFlag(FLAGS_http_port);
  std::vector<std::string> server_args = {
      "--problem_config=" + effective.string(),
      "--data_dir=" + data_dir.string(),
      absl::StrCat("--grpc_port=", grpc_port),
      absl::StrCat("--http_port=", http_port),
  };
  if (gated) {
    server_args.push_back("--clients=" + clients.string());
  }

  struct sigaction action {};
  action.sa_handler = OnStopSignal;
  ::sigaction(SIGINT, &action, nullptr);
  ::sigaction(SIGTERM, &action, nullptr);

  auto server = process::Child::Start(server_bin.string(), server_args,
                                      process::ChildOptions{});
  if (!server) {
    LOG(ERROR) << "cannot start " << server_bin;
    return 1;
  }
  if (!WaitForPort(grpc_port, std::chrono::seconds(60))) {
    LOG(ERROR) << "problem_server did not open port " << grpc_port;
    server->Stop(std::chrono::seconds(5));
    return 1;
  }

  std::vector<process::Child> workers;
  const int worker_count = std::max(1, absl::GetFlag(FLAGS_workers));
  for (int i = 0; i < worker_count; ++i) {
    process::ChildOptions options;
    options.extra_env = {
        "ARENA_WORK_DIR=" + (data_dir / "work" / std::to_string(i)).string(),
        "ARENA_SLOTS=" + EnvOr("ARENA_SLOTS", "1"),
        absl::StrCat("ARENA_WORKER_ID=local-", i),
    };
    auto worker = process::Child::Start(
        worker_bin.string(), {absl::StrCat("--server=localhost:", grpc_port)},
        options);
    if (!worker) {
      LOG(ERROR) << "cannot start " << worker_bin;
      server->Stop(std::chrono::seconds(5));
      return 1;
    }
    workers.push_back(std::move(*worker));
  }

  std::printf(
      "\n"
      "%s is up.\n"
      "  leaderboard   http://localhost:%d/\n"
      "  arena         localhost:%d   (%s)\n"
      "  state         %s\n"
      "  workers       %d local, %s\n"
      "\n"
      "A participant's kit:\n"
      "  bazel run //:kit -- --mint=<client_id> --server=<this host>:%d\n"
      "\n"
      "Ctrl-C stops everything.\n\n",
      config->display_name().empty() ? config->problem_id().c_str()
                                     : config->display_name().c_str(),
      http_port, grpc_port,
      gated ? "writes need a token from the registry" : "writes open",
      data_dir.c_str(), worker_count,
      config->sandbox().image().empty() ? "process engine (unsandboxed)"
                                        : "docker engine",
      grpc_port);
  std::fflush(stdout);

  int status = 0;
  while (!g_stop_requested) {
    if (const auto code = server->Poll()) {
      LOG(ERROR) << "problem_server exited with " << *code;
      status = 1;
      break;
    }
    bool worker_died = false;
    for (auto &worker : workers) {
      if (const auto code = worker.Poll()) {
        LOG(ERROR) << "a sandbox_worker exited with " << *code;
        worker_died = true;
      }
    }
    if (worker_died) {
      status = 1;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }

  // Workers first: an order in flight is cancelled rather than orphaned, and
  // the coordinator sees its stream close.
  for (auto &worker : workers) {
    worker.Stop(std::chrono::seconds(10));
  }
  server->Stop(std::chrono::seconds(10));
  return status;
}

// ---------------------------------------------------------------------------
// kit
// ---------------------------------------------------------------------------

// The problem's MODULE.bazel, with every relative local_path_override re-rooted
// so it still points where it did from the kit's new location.
auto RerootedModuleFile(std::string text,
                        const std::filesystem::path &root) -> std::string {
  static const std::regex kOverride(
      R"re(local_path_override\(([^)]*?)path\s*=\s*"([^"]+)")re");
  std::string out;
  auto begin = std::sregex_iterator(text.begin(), text.end(), kOverride);
  std::size_t last = 0;
  for (auto it = begin; it != std::sregex_iterator(); ++it) {
    const std::smatch &m = *it;
    const std::filesystem::path path(m[2].str());
    out.append(text, last, m.position(0) - last);
    if (path.is_absolute()) {
      out.append(m[0].str());
    } else {
      out.append("local_path_override(")
          .append(m[1].str())
          .append("path = \"")
          .append((root / path).lexically_normal().string())
          .append("\"");
    }
    last = m.position(0) + m.length(0);
  }
  out.append(text, last, std::string::npos);
  return out;
}

auto KitBuildFile(const std::string &registry) -> std::string {
  std::string text =
      "# Generated by arena_tournament kit. The arena's tools, reachable from\n"
      "# this workspace through @game_arena";
  if (!registry.empty()) {
    text += ", and a broker to play against locally";
  }
  text += ".\n\n";
  if (!registry.empty()) {
    text += "load(\"@rules_cc//cc:cc_binary.bzl\", \"cc_binary\")\n\n";
  }
  text +=
      "package(default_visibility = [\"//visibility:public\"])\n"
      "\n"
      "# Submit, poll, read standings and rivals' source. Reads $ARENA_SERVER\n"
      "# and $ARENA_TOKEN (see arena.env).\n"
      "alias(\n"
      "    name = \"arena_cli\",\n"
      "    actual = \"@game_arena//game_arena/tools:arena_cli\",\n"
      ")\n"
      "\n"
      "# The same, as MCP tools for an agent (see mcp.json).\n"
      "alias(\n"
      "    name = \"mcp_server\",\n"
      "    actual = \"@game_arena//mcp_servers/arena_mcp:server\",\n"
      ")\n";
  if (!registry.empty()) {
    text += absl::StrCat(
        "\n"
        "# A long-running broker with this problem's rules, for iterating: "
        "run\n"
        "# it, then point your bot at it with --opponent=builtin:<name>.\n"
        "cc_binary(\n"
        "    name = \"broker_server\",\n"
        "    deps = [\n"
        "        \"",
        registry,
        "\",\n"
        "        \"@game_arena//game_arena/referee:broker_server_main\",\n"
        "    ],\n"
        ")\n"
        "\n"
        "# A player making uniformly random legal moves; a floor to beat.\n"
        "cc_binary(\n"
        "    name = \"random_client\",\n"
        "    deps = [\n"
        "        \"",
        registry,
        "\",\n"
        "        \"@game_arena//game_arena/client:random_client_main\",\n"
        "    ],\n"
        ")\n");
  }
  return text;
}

auto KitReadme(const proto::ProblemConfig &config, const std::string &server,
               const std::string &http, const std::string &client_id,
               bool has_token,
               const std::vector<std::string> &files) -> std::string {
  std::ostringstream md;
  const std::string title = config.display_name().empty()
                                ? config.problem_id()
                                : config.display_name();
  md << "# " << title << "\n\n" << config.description() << "\n";

  md << "## Your solution\n\n";
  const auto &submission = config.submission();
  if (!submission.files_submit_dir().empty()) {
    md << "A submission is a set of files; the arena places them under `"
       << submission.files_submit_dir()
       << "/<your-submission-id>/` and generates the BUILD file. ";
    if (!submission.harness().api_dep().empty()) {
      md << "They are compiled against `" << submission.harness().api_dep()
         << "`";
      if (!submission.allowed_dep_prefixes().empty()) {
        md << " and may additionally depend on labels under `"
           << absl::StrJoin(submission.allowed_dep_prefixes(), "`, `") << "`";
      }
      md << ".";
    }
    md << "\n";
  } else {
    md << "A submission is a unified diff against the problem's base commit.\n";
  }
  if (!submission.allow_paths().empty()) {
    md << "A patch may only touch: `"
       << absl::StrJoin(submission.allow_paths(), "`, `") << "`.\n";
  }
  md << "\nThis kit holds what a solution is written against:\n\n";
  for (const std::string &file : files) {
    md << "- `" << file << "`\n";
  }

  md << "\n## Iterating locally\n\n```sh\nbazel build //...\n";
  if (config.has_match()) {
    md << "bazel run //:broker_server -- --grpc_port=50051 --http_port=8080 &\n"
          "# then run your bot (see the harness in this kit) against it with\n"
          "#   --server=localhost:50051 --opponent=builtin:<name>\n";
    if (!config.match().placement_opponents().empty()) {
      md << "# builtins the arena rates you against first: "
         << absl::StrJoin(config.match().placement_opponents(), ", ") << "\n";
    }
  }
  md << "```\n";

  md << "\n## Submitting\n\n"
        "The arena is at `"
     << server << "`; the leaderboard at <http://" << http << "/>.";
  if (has_token) {
    md << " Your token is in `arena.env` and `mcp.json`, and identifies you as "
          "`"
       << client_id << "`.";
  }
  md << "\n\n```sh\n. ./arena.env\n"
        "bazel run //:arena_cli -- rules\n"
        "bazel run //:arena_cli -- submit --name=\"My bot\" --file=<path> "
        "--wait\n"
        "bazel run //:arena_cli -- leaderboard\n"
        "bazel run //:arena_cli -- source <candidate_id>   # any rival's "
        "source is readable\n"
        "```\n\n"
        "For an agent, `mcp.json` registers the same operations as MCP tools "
        "(`arena_rules`, `arena_submit`, `arena_job`, `arena_leaderboard`, "
        "`arena_source`, `arena_evaluate`); `arena_submit` takes file paths "
        "relative to this directory.\n";
  return md.str();
}

auto JsonEscape(std::string_view s) -> std::string {
  std::string out;
  for (const char c : s) {
    if (c == '"' || c == '\\') {
      out.push_back('\\');
    }
    out.push_back(c);
  }
  return out;
}

auto RunKit() -> int {
  std::filesystem::path config_path;
  const auto config = LoadConfig(&config_path);
  if (!config) {
    return 1;
  }
  const std::filesystem::path root = WorkspaceRoot();

  // The files, from the macro's environment (rootpaths, space-separated) plus
  // any positional arguments.
  std::vector<std::string> files =
      absl::StrSplit(EnvOr("ARENA_KIT_FILES", ""), ' ', absl::SkipWhitespace());
  for (const std::string &file : files) {
    if (file.rfind("../", 0) == 0 || file.rfind("bazel-out/", 0) == 0 ||
        std::filesystem::path(file).is_absolute()) {
      LOG(ERROR) << "kit_files must be source files of this repository: "
                 << file;
      return 1;
    }
    if (file == "BUILD" || file == "BUILD.bazel") {
      LOG(ERROR) << "the kit's root BUILD is generated; a root BUILD in "
                    "kit_files would collide with it";
      return 1;
    }
    if (!std::filesystem::is_regular_file(root / file)) {
      LOG(ERROR) << "kit file not found: " << (root / file).string();
      return 1;
    }
  }

  const std::string client_id = absl::GetFlag(FLAGS_mint);
  const std::filesystem::path out =
      absl::GetFlag(FLAGS_out).empty()
          ? StateDir(config->problem_id()) / "kits" /
                (client_id.empty() ? "participant" : client_id)
          : Resolve(absl::GetFlag(FLAGS_out));
  if (std::filesystem::exists(out) && !std::filesystem::is_empty(out) &&
      !absl::GetFlag(FLAGS_force)) {
    LOG(ERROR) << out
               << " exists and is not empty; pass --force to write "
                  "into it anyway";
    return 1;
  }
  std::error_code ec;
  std::filesystem::create_directories(out, ec);
  if (ec) {
    LOG(ERROR) << "cannot create " << out << ": " << ec.message();
    return 1;
  }

  // The token.
  std::string token = absl::GetFlag(FLAGS_token);
  if (!client_id.empty()) {
    if (!token.empty()) {
      LOG(ERROR) << "--mint and --token are exclusive";
      return 1;
    }
    const std::filesystem::path clients =
        absl::GetFlag(FLAGS_clients).empty()
            ? StateDir(config->problem_id()) / "clients.textproto"
            : Resolve(absl::GetFlag(FLAGS_clients));
    std::filesystem::create_directories(clients.parent_path(), ec);
    token = tournament_arena::MintToken();
    std::string error;
    if (!tournament_arena::AppendClientToRegistry(
            clients,
            tournament_arena::MakeClient(client_id, client_id, token,
                                         proto::ClientQuota()),
            &error)) {
      LOG(ERROR) << error;
      return 1;
    }
    std::printf("Minted a token for '%s' into %s.\n", client_id.c_str(),
                clients.c_str());
    std::printf(
        "A coordinator already running on that registry needs a SIGHUP; one "
        "started without --clients accepts any token.\n");
  }

  // The workspace skeleton, as the problem has it.
  const auto module = ReadFile(root / "MODULE.bazel");
  if (!module) {
    LOG(ERROR) << "no MODULE.bazel at " << root
               << "; run this from the problem's workspace";
    return 1;
  }
  if (module->find("game_arena") == std::string::npos) {
    LOG(WARNING) << "MODULE.bazel does not mention game_arena; the kit's "
                    "arena_cli and mcp_server aliases will not resolve";
  }
  WriteFile(out / "MODULE.bazel", RerootedModuleFile(*module, root));
  for (const char *name : {"MODULE.bazel.lock", ".bazelversion", ".bazelrc"}) {
    if (const auto text = ReadFile(root / name)) {
      WriteFile(out / name, *text);
    }
  }
  for (const std::string &file : files) {
    std::filesystem::create_directories((out / file).parent_path(), ec);
    std::filesystem::copy_file(
        root / file, out / file,
        std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) {
      LOG(ERROR) << "cannot copy " << file << ": " << ec.message();
      return 1;
    }
  }

  const std::string server = absl::GetFlag(FLAGS_server);
  const std::string http = absl::GetFlag(FLAGS_http);
  const std::string registry = absl::GetFlag(FLAGS_registry);

  WriteFile(out / "BUILD", KitBuildFile(registry));
  WriteFile(out / "ARENA.md",
            KitReadme(*config, server, http, client_id, !token.empty(), files));

  std::string env = "export ARENA_SERVER=" + server + "\n";
  if (!token.empty()) {
    env += "export ARENA_TOKEN=" + token + "\n";
  }
  WriteFile(out / "arena.env", env);

  std::string mcp = absl::StrCat(
      "{\n"
      "  \"mcpServers\": {\n"
      "    \"arena\": {\n"
      "      \"command\": \"bazel\",\n"
      "      \"args\": [\"run\", \"//:mcp_server\"],\n"
      "      \"cwd\": \"",
      JsonEscape(out.string()),
      "\",\n"
      "      \"env\": {\n"
      "        \"ARENA_MCP_TARGET\": \"",
      JsonEscape(server), "\"");
  if (!token.empty()) {
    absl::StrAppend(&mcp, ",\n        \"ARENA_MCP_TOKEN\": \"",
                    JsonEscape(token), "\"");
  }
  if (!client_id.empty()) {
    absl::StrAppend(&mcp, ",\n        \"ARENA_MCP_AUTHOR\": \"",
                    JsonEscape(client_id), "\"");
  }
  absl::StrAppend(&mcp, "\n      }\n    }\n  }\n}\n");
  WriteFile(out / "mcp.json", mcp);

  WriteFile(out / ".gitignore", "bazel-*\n.bazelrc.local\n");
  const std::string arena_override = absl::GetFlag(FLAGS_arena_override);
  if (!arena_override.empty()) {
    WriteFile(out / ".bazelrc.local",
              absl::StrCat("# Written by arena_tournament kit --arena_override."
                           "\ncommon --override_module=game_arena=",
                           Resolve(arena_override).string(), "\n"));
    const auto rc = ReadFile(out / ".bazelrc");
    if (!rc || rc->find(".bazelrc.local") == std::string::npos) {
      WriteFile(out / ".bazelrc",
                absl::StrCat(rc.value_or(""),
                             "\ntry-import %workspace%/.bazelrc.local\n"));
    }
  }

  std::printf("Kit for %s written to %s:\n", config->problem_id().c_str(),
              out.c_str());
  std::printf("  ARENA.md      the rules, and how to submit\n");
  std::printf("  arena.env     ARENA_SERVER%s for arena_cli\n",
              token.empty() ? "" : " and ARENA_TOKEN");
  std::printf("  mcp.json      the MCP server for an agent\n");
  std::printf("  %zu file(s) from the problem\n", files.size());
  if (!token.empty()) {
    std::printf(
        "\nThe token is inside the kit; hand the directory to one "
        "participant only.\n");
  }

  if (absl::GetFlag(FLAGS_check)) {
    std::printf("\nChecking that the kit builds on its own...\n");
    std::fflush(stdout);
    const int code = RunInherit("bazel", {"build", "//..."}, out);
    if (code != 0) {
      LOG(ERROR) << "the kit does not build (exit " << code
                 << "); kit_files is probably missing a BUILD file or a "
                    "source one of them depends on";
      return 1;
    }
    std::printf("Kit builds.\n");
  }
  return 0;
}

// ---------------------------------------------------------------------------
// image
// ---------------------------------------------------------------------------

// Where the problem overrides game_arena with a local checkout, if it does:
// MODULE.bazel's local_path_override, or --override_module in .bazelrc.local.
auto LocalArenaOverride(const std::filesystem::path &root)
    -> std::optional<std::filesystem::path> {
  if (const auto module = ReadFile(root / "MODULE.bazel")) {
    static const std::regex kOverride(
        R"re(local_path_override\(\s*module_name\s*=\s*"game_arena"[^)]*?path\s*=\s*"([^"]+)")re");
    std::smatch m;
    if (std::regex_search(*module, m, kOverride)) {
      return (root / m[1].str()).lexically_normal();
    }
  }
  for (const char *rc : {".bazelrc.local", ".bazelrc"}) {
    if (const auto text = ReadFile(root / rc)) {
      static const std::regex kFlag(R"(--override_module=game_arena=(\S+))");
      std::smatch m;
      if (std::regex_search(*text, m, kFlag)) {
        return (root / m[1].str()).lexically_normal();
      }
    }
  }
  return std::nullopt;
}

auto RunImage(const ArenaRunfiles &runfiles) -> int {
  std::filesystem::path config_path;
  const auto config = LoadConfig(&config_path);
  if (!config) {
    return 1;
  }
  const std::string tag = absl::GetFlag(FLAGS_tag).empty()
                              ? config->sandbox().image()
                              : absl::GetFlag(FLAGS_tag);
  if (tag.empty()) {
    LOG(ERROR) << "sandbox.image is empty and no --tag given: nothing to "
                  "build";
    return 1;
  }
  const std::filesystem::path dockerfile =
      runfiles.Locate("game_arena/sandbox/image/Dockerfile");
  if (dockerfile.empty()) {
    return 1;
  }
  const std::filesystem::path root = WorkspaceRoot();
  if (!std::filesystem::exists(root / "MODULE.bazel")) {
    LOG(ERROR) << "no MODULE.bazel at " << root
               << "; the image is built from the problem's workspace";
    return 1;
  }

  std::string bazel_version = absl::GetFlag(FLAGS_bazel_version);
  if (bazel_version.empty()) {
    if (const auto text = ReadFile(root / ".bazelversion")) {
      const std::string trimmed(absl::StripAsciiWhitespace(*text));
      static const std::regex kRelease(R"(\d+\.\d+\.\d+)");
      if (std::regex_match(trimmed, kRelease)) {
        bazel_version = trimmed;
      }
    }
  }

  std::vector<std::string> args = {"build", "--file", dockerfile.string(),
                                   "--tag", tag};
  if (!bazel_version.empty()) {
    args.push_back("--build-arg");
    args.push_back("BAZEL_VERSION=" + bazel_version);
  }
  if (const auto override = LocalArenaOverride(root)) {
    LOG(WARNING) << "game_arena is overridden with the local checkout "
                 << override->string()
                 << "; a copy of it goes into the image. Pin a git commit in "
                    "MODULE.bazel for an image that does not depend on this "
                    "host";
    args.push_back("--build-context");
    args.push_back("game_arena=" + override->string());
  }
  args.push_back(root.string());

  std::printf("docker %s\n", absl::StrJoin(args, " ").c_str());
  std::fflush(stdout);
  const std::string docker = absl::GetFlag(FLAGS_docker);
  int code = RunInherit(docker, args, root);
  if (code != 0) {
    LOG(ERROR) << "docker build failed (exit " << code << ")";
    return 1;
  }
  if (absl::GetFlag(FLAGS_push)) {
    code = RunInherit(docker, {"push", tag}, root);
    if (code != 0) {
      LOG(ERROR) << "docker push failed (exit " << code << ")";
      return 1;
    }
  }

  std::printf("\nBuilt %s. Smoke test it offline:\n\n", tag.c_str());
  std::printf("  docker run --rm --network=none -v %s:/src:ro -w /src %s \\\n",
              root.c_str(), tag.c_str());
  std::printf("      bazel --output_base=/tmp/ob build %s\n",
              absl::StrJoin(config->build().targets(), " ").c_str());
  return 0;
}

void PrintUsage() {
  std::fprintf(stderr,
               "usage: arena_tournament <up|kit|check|image> "
               "--problem_config=<path> [flags]\n"
               "  up      run a coordinator and local workers\n"
               "  kit     write a participant's workspace (--out, --mint)\n"
               "  check   validate the config\n"
               "  image   build the sandbox image (docker)\n");
}

}  // namespace

auto main(int argc, char **argv) -> int {
  const std::vector<char *> positional = absl::ParseCommandLine(argc, argv);
  absl::InitializeLog();
  absl::SetStderrThreshold(absl::LogSeverityAtLeast::kInfo);

  if (positional.size() < 2) {
    PrintUsage();
    return 2;
  }
  const std::string command = positional[1];
  const ArenaRunfiles runfiles(argv[0]);
  if (command == "up") {
    return RunUp(runfiles);
  }
  if (command == "kit") {
    return RunKit();
  }
  if (command == "check") {
    return RunCheck();
  }
  if (command == "image") {
    return RunImage(runfiles);
  }
  PrintUsage();
  return 2;
}
