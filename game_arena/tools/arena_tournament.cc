// Standing a tournament up from a problem repository, and handing participants
// a kit to enter it.
/*
bazel run //:tournament -- [--workers=2]
bazel run //:play                            # all of it, and a shell in your
kit bazel run //:kit -- --out=/srv/kits/alice --server=arena:50051 --mint=alice
bazel run //:kit -- --mint=bob --server=arena:50051 --image=registry/kit-bob
bazel run //:tournament -- --image=registry/c4-arena --push
bazel run //:sandbox_image -- [--push]
bazel test //:config_test

Without the macro, from a problem repo:
bazel run @game_arena//game_arena/tools:arena_tournament -- \
    up --problem_config=problem.textproto
*/
//
// Four subcommands, one problem config:
//
//   up      a coordinator and N local workers on this checkout, building the
//           problem's sandbox image first if this daemon does not have it.
//           Every submission is built and run in a container -- there is no
//           mode that skips that. With --image, the same as a docker image:
//           the arena's binaries and the problem, to `docker run` on any host
//           with a docker socket.
//   kit     a participant's workspace: the files the problem names, the
//           arena's CLI and MCP server reachable through @game_arena, a README
//           from the config, and a freshly minted token. With --image, the
//           same as a docker image with the toolchain and everything built,
//           ready to `docker run` wherever the participant works.
//   check   the config parses and is consistent with the tree. What
//           :config_test runs.
//   image   the problem's sandbox image: toolchain + vendored deps, so builds
//           run with no network.
//   play    up, in the background with its logs in a file, a kit minted for
//           you, and a shell in it with the arena's address and your token in
//           the environment. Leaving the shell stops everything. The dev loop.
//
// Inside the tournament image the tool runs installed, not under bazel:
// ARENA_HOME points at the arena's files laid out as in its tree, and
// ARENA_PROBLEM_CONFIG names the config, so `arena_tournament up` and `kit`
// need no flags there.
//
// The tool knows what a problem *config* is, never what a problem is made of.
// Every label, file and name here arrives from the caller.

#include <arpa/inet.h>
#include <google/protobuf/text_format.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
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
#include "game_arena/proto/kit.pb.h"
#include "game_arena/server/client_registry.h"
#include "game_arena/server/problem_config.h"
#include "rules_cc/cc/runfiles/runfiles.h"

ABSL_FLAG(std::string, problem_config, "",
          "The problem's .textproto (required; default $ARENA_PROBLEM_CONFIG). "
          "Relative to the workspace root under `bazel run`, else to the "
          "current directory");

// up
ABSL_FLAG(std::string, data_dir, "",
          "up: where the coordinator keeps submissions, ratings and the "
          "workers' checkouts. Default: $ARENA_STATE_DIR/<problem_id>, and "
          "$ARENA_STATE_DIR defaults to ~/.arena. Kept out of the repo so "
          "`bazel test //...` there never descends into a worker's clone");
ABSL_FLAG(int, grpc_port, 50051, "up: the Arena and SandboxFleet port");
ABSL_FLAG(int, http_port, 8090, "up: the leaderboard port");
ABSL_FLAG(int, workers, 1, "up: local sandbox workers to start");
ABSL_FLAG(std::string, clients, "",
          "up/kit: the client registry. up passes it to the coordinator, "
          "creating it empty if it does not exist; kit --mint appends to it "
          "and tells the coordinator to reload. Default: "
          "<data_dir>/clients.textproto");

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
          "broker in the kit. Supplied by the arena_problem macro; default "
          "$ARENA_KIT_REGISTRY");
ABSL_FLAG(std::string, arena_override, "",
          "kit: write a .bazelrc.local pointing @game_arena at this local "
          "checkout, for a participant on the same host");
ABSL_FLAG(std::string, cache_limit, "5G",
          "kit: how large the kit's own bazel disk cache may grow before "
          "bazel collects it");
ABSL_FLAG(bool, prime_cache, true,
          "kit: build the kit once when it is written, keeping the result in "
          "a bazel disk cache inside it, so a participant's first build is "
          "warm and a missing kit file is found here rather than by them");
ABSL_FLAG(bool, force, false, "kit: write into a non-empty --out");
ABSL_FLAG(std::string, image, "",
          "kit: also build a docker image of the kit with this tag: the "
          "toolchain, the kit, and a completed `bazel build //...`. "
          "up: instead of running, build a docker image of the tournament "
          "with this tag: the coordinator, the workers and the problem, for "
          "any host with a docker socket. --push pushes either");

// play
ABSL_FLAG(std::string, shell, "",
          "play: the shell to drop into the kit. Default: $SHELL, else bash");

// image
ABSL_FLAG(std::string, tag, "",
          "image: tag to build. Default: the config's sandbox.image");
ABSL_FLAG(bool, push, false, "image, --image: docker push the result");
ABSL_FLAG(std::string, bazel_version, "",
          "image, --image: bazel release to install. Default: the repo's "
          ".bazelversion when it names a release, else the Dockerfile's");
ABSL_FLAG(std::string, docker, "docker", "image, --image: the docker binary");

namespace {

namespace proto = tournament_arena::proto;
using rules_cc::cc::runfiles::Runfiles;

volatile std::sig_atomic_t g_stop_requested = 0;
volatile std::sig_atomic_t g_reload_requested = 0;

extern "C" void OnStopSignal(int /*signum*/) { g_stop_requested = 1; }
extern "C" void OnReloadSignal(int /*signum*/) { g_reload_requested = 1; }

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
  const std::string flag = absl::GetFlag(FLAGS_problem_config).empty()
                               ? EnvOr("ARENA_PROBLEM_CONFIG", "")
                               : absl::GetFlag(FLAGS_problem_config);
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

// The arena's files: under bazel, from runfiles; installed (the tournament
// image), from $ARENA_HOME, where they are laid out as in the arena's tree.
class ArenaRunfiles {
 public:
  explicit ArenaRunfiles(const char *argv0) : home_(EnvOr("ARENA_HOME", "")) {
    std::string error;
    runfiles_.reset(Runfiles::Create(argv0, BAZEL_CURRENT_REPOSITORY, &error));
    if (!runfiles_ && home_.empty()) {
      LOG(WARNING) << "runfiles unavailable: " << error;
    }
  }

  // A file of the game_arena module, by workspace-relative path.
  auto Locate(const std::string &path) const -> std::filesystem::path {
    std::vector<std::string> candidates;
    if (!home_.empty()) {
      candidates.push_back((std::filesystem::path(home_) / path).string());
    }
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

  // A *source* file or directory of the game_arena module: the same lookup,
  // but installed the arena's sources are under $ARENA_HOME/src/game_arena
  // (its binaries are laid out at $ARENA_HOME directly, so Locate would find
  // the wrong thing or nothing). Empty when it is not there, which is not an
  // error by itself -- only the kit's vendored arena needs sources.
  auto LocateSource(const std::string &path) const -> std::filesystem::path {
    if (!home_.empty()) {
      const std::filesystem::path src =
          std::filesystem::path(home_) / "src" / "game_arena" / path;
      if (std::filesystem::exists(src)) {
        return std::filesystem::absolute(src);
      }
    }
    std::vector<std::string> candidates;
    if (runfiles_) {
      candidates.push_back(runfiles_->Rlocation("game_arena/" + path));
      candidates.push_back(runfiles_->Rlocation("_main/" + path));
    }
    const std::string dir = EnvOr("RUNFILES_DIR", "");
    if (!dir.empty()) {
      candidates.push_back(dir + "/game_arena+/" + path);
      candidates.push_back(dir + "/_main/" + path);
    }
    for (const std::string &candidate : candidates) {
      if (!candidate.empty() && std::filesystem::exists(candidate)) {
        return std::filesystem::absolute(candidate);
      }
    }
    return {};
  }

  // Installed rather than under bazel.
  auto installed() const -> bool { return !home_.empty(); }

 private:
  std::string home_;
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
  // A sandbox builds with no network, so the module graph has to be settled
  // in the tree the worker clones: a lockfile that is not committed is a
  // build that re-resolves against the registry and fails.
  const auto repo = LocalRepoDir(config);
  if (!config.sandbox().allow_build_network() && repo &&
      std::filesystem::is_directory(*repo / ".git")) {
    const auto tracked = Capture(
        "git", {"ls-files", "--error-unmatch", "MODULE.bazel.lock"}, *repo);
    if (!tracked) {
      *error = absl::StrCat(
          "MODULE.bazel.lock is not committed in ", repo->string(),
          "; the sandbox builds without a network and needs it. Run a build, "
          "then `git add MODULE.bazel.lock`");
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
// docker
// ---------------------------------------------------------------------------

// Where the problem overrides game_arena with a local checkout, if it does:
// --override_module in .bazelrc.local (which, as a flag, beats the file), or
// MODULE.bazel's local_path_override.
auto LocalArenaOverride(const std::filesystem::path &root)
    -> std::optional<std::filesystem::path> {
  for (const char *rc : {".bazelrc.local", ".bazelrc"}) {
    if (const auto text = ReadFile(root / rc)) {
      static const std::regex kFlag(R"(--override_module=game_arena=(\S+))");
      std::smatch m;
      if (std::regex_search(*text, m, kFlag)) {
        return (root / m[1].str()).lexically_normal();
      }
    }
  }
  if (const auto module = ReadFile(root / "MODULE.bazel")) {
    static const std::regex kOverride(
        R"re(local_path_override\(\s*module_name\s*=\s*"game_arena"[^)]*?path\s*=\s*"([^"]+)")re");
    std::smatch m;
    if (std::regex_search(*module, m, kOverride)) {
      return (root / m[1].str()).lexically_normal();
    }
  }
  return std::nullopt;
}

// The bazel release the repo pins in .bazelversion, if it names one, else
// --bazel_version, else empty (the Dockerfile's default).
auto BazelVersionFor(const std::filesystem::path &root) -> std::string {
  std::string version = absl::GetFlag(FLAGS_bazel_version);
  if (version.empty()) {
    if (const auto text = ReadFile(root / ".bazelversion")) {
      const std::string trimmed(absl::StripAsciiWhitespace(*text));
      static const std::regex kRelease(R"(\d+\.\d+\.\d+)");
      if (std::regex_match(trimmed, kRelease)) {
        version = trimmed;
      }
    }
  }
  return version;
}

// `docker build --target |target|` of the arena's Dockerfile with |context| as
// the build context, tagged |tag|, then `docker push` under --push. A local
// game_arena override found in |context| goes in as a second build context,
// since its host path does not exist inside the build. |build_args| are
// NAME=value pairs; |contexts| further name=path build contexts.
auto DockerBuild(const std::filesystem::path &dockerfile,
                 const std::string &target, const std::string &tag,
                 const std::filesystem::path &context,
                 const std::vector<std::string> &build_args,
                 const std::vector<std::string> &contexts = {},
                 bool push = absl::GetFlag(FLAGS_push),
                 bool with_arena_context = true) -> bool {
  std::vector<std::string> args = {"build", "--file", dockerfile.string(),
                                   "--tag", tag};
  if (!target.empty()) {
    args.push_back("--target");
    args.push_back(target);
  }
  for (const std::string &named : contexts) {
    args.push_back("--build-context");
    args.push_back(named);
  }
  const std::string bazel_version = BazelVersionFor(context);
  if (!bazel_version.empty()) {
    args.push_back("--build-arg");
    args.push_back("BAZEL_VERSION=" + bazel_version);
  }
  for (const std::string &arg : build_args) {
    args.push_back("--build-arg");
    args.push_back(arg);
  }
  if (const auto override =
          with_arena_context ? LocalArenaOverride(context) : std::nullopt) {
    if (!std::filesystem::exists(*override / "MODULE.bazel")) {
      LOG(ERROR) << "game_arena is overridden with " << override->string()
                 << ", which is not a bazel module here. Pin a git commit in "
                    "MODULE.bazel, or point --arena_override at a checkout "
                    "that exists on this host";
      return false;
    }
    LOG(WARNING) << "game_arena is overridden with the local checkout "
                 << override->string()
                 << "; a copy of it goes into the image. Pin a git commit in "
                    "MODULE.bazel for an image that does not depend on this "
                    "host";
    args.push_back("--build-context");
    args.push_back("game_arena=" + override->string());
  }
  args.push_back(context.string());

  std::printf("docker %s\n", absl::StrJoin(args, " ").c_str());
  std::fflush(stdout);
  const std::string docker = absl::GetFlag(FLAGS_docker);
  int code = RunInherit(docker, args, context);
  if (code != 0) {
    LOG(ERROR) << "docker build failed (exit " << code << ")";
    return false;
  }
  if (push) {
    code = RunInherit(docker, {"push", tag}, context);
    if (code != 0) {
      LOG(ERROR) << "docker push failed (exit " << code << ")";
      return false;
    }
  }
  return true;
}

// ---------------------------------------------------------------------------
// up
// ---------------------------------------------------------------------------

auto Hostname() -> std::string {
  char name[256] = {};
  if (::gethostname(name, sizeof(name) - 1) != 0) {
    return "host";
  }
  return name;
}

// Runs a command with its output discarded; the exit code, -1 if it could not
// start.
auto RunQuiet(const std::string &executable,
              const std::vector<std::string> &arguments) -> int {
  process::ChildOptions options;
  options.stdout_path = "/dev/null";
  options.stderr_path = "/dev/null";
  auto child = process::Child::Start(executable, arguments, options);
  return child ? child->Wait() : -1;
}

// The tournament as an image: the arena's binaries built from this workspace,
// the repo for the workers to clone, and the sandbox reached through a
// mounted docker socket. Built from the problem's workspace root, as the
// sandbox image is.
auto BuildTournamentImage(const ArenaRunfiles &runfiles,
                          const proto::ProblemConfig &config,
                          const std::filesystem::path &config_path,
                          const std::string &tag) -> int {
  const std::filesystem::path dockerfile =
      runfiles.Locate("game_arena/image/Dockerfile");
  if (dockerfile.empty()) {
    return 1;
  }
  const std::filesystem::path root = WorkspaceRoot();
  if (!std::filesystem::exists(root / "MODULE.bazel") ||
      !std::filesystem::exists(root / ".git")) {
    LOG(ERROR) << root
               << " is not a git repository with a MODULE.bazel; the "
                  "tournament image is built from the problem's workspace, "
                  "and its workers clone it";
    return 1;
  }
  if (config.sandbox().image().empty()) {
    LOG(ERROR) << "sandbox.image is empty. The tournament image carries no "
                  "toolchain: its workers build every submission in the "
                  "sandbox image, so the problem has to name one";
    return 1;
  }
  std::error_code ec;
  const std::filesystem::path config_rel =
      std::filesystem::relative(config_path, root, ec);
  if (ec || config_rel.empty() || config_rel.string().rfind("..", 0) == 0) {
    LOG(ERROR) << "the config " << config_path << " is not inside " << root;
    return 1;
  }
  const std::string registry = absl::GetFlag(FLAGS_registry).empty()
                                   ? EnvOr("ARENA_KIT_REGISTRY", "")
                                   : absl::GetFlag(FLAGS_registry);
  std::printf(
      "Building %s from %s (the arena's binaries, from this workspace)...\n",
      tag.c_str(), root.c_str());
  std::fflush(stdout);
  if (!DockerBuild(dockerfile, "tournament", tag, root,
                   {"PROBLEM_ID=" + config.problem_id(),
                    "PROBLEM_CONFIG=" + config_rel.string(),
                    "ARENA_KIT_FILES=" + EnvOr("ARENA_KIT_FILES", ""),
                    "ARENA_KIT_REGISTRY=" + registry},
                   {"arena_image=" + dockerfile.parent_path().string()})) {
    return 1;
  }
  const std::string name = config.problem_id() + "-arena";
  std::printf(
      "\nBuilt %s%s. Run it on any host with a docker socket and the sandbox "
      "image %s on that daemon:\n\n"
      "  docker run -d --name %s --restart=unless-stopped \\\n"
      "      -p 50051:50051 -p 8090:8090 \\\n"
      "      -v /var/run/docker.sock:/var/run/docker.sock -v %s:/var/arena "
      "%s\n\n"
      "Then, per participant (mints a token and reloads the registry):\n\n"
      "  docker exec -it %s arena_tournament kit --mint=<client_id> "
      "--server=<host>:50051 [--image=REG/kit-<client_id> --push]\n\n"
      "And one more worker, anywhere with a socket and the sandbox image:\n\n"
      "  docker run -d -v /var/run/docker.sock:/var/run/docker.sock "
      "-e ARENA_VOLUME_PREFIX=<unique> %s sandbox_worker "
      "--server=<host>:50051\n",
      tag.c_str(), absl::GetFlag(FLAGS_push) ? " and pushed it" : "",
      config.sandbox().image().c_str(), name.c_str(), name.c_str(), tag.c_str(),
      name.c_str(), tag.c_str());
  return 0;
}

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

  if (!absl::GetFlag(FLAGS_image).empty()) {
    return BuildTournamentImage(runfiles, *config, config_path,
                                absl::GetFlag(FLAGS_image));
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

  // Submitted code runs in a container or it does not run: there is no flag
  // here that turns that off. A tournament without its sandbox image is a
  // tournament that cannot build anything, so build it rather than starting
  // and failing every order.
  const std::string &image = config->sandbox().image();
  if (process::ResolveExecutable(absl::GetFlag(FLAGS_docker)).empty()) {
    LOG(ERROR) << "no " << absl::GetFlag(FLAGS_docker)
               << " on PATH. A tournament builds and runs every submission in "
                  "a container; there is no unsandboxed mode";
    return 1;
  }
  if (RunQuiet(absl::GetFlag(FLAGS_docker), {"image", "inspect", image}) != 0) {
    std::printf(
        "The sandbox image %s is not on this docker daemon; building it "
        "(the first time takes a while)...\n",
        image.c_str());
    std::fflush(stdout);
    const std::filesystem::path dockerfile =
        runfiles.Locate("game_arena/image/Dockerfile");
    if (dockerfile.empty() ||
        !DockerBuild(dockerfile, "sandbox", image, WorkspaceRoot(), {})) {
      LOG(ERROR) << "cannot build the sandbox image " << image
                 << "; build or pull it, then start the tournament again";
      return 1;
    }
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

  // Always gated: an empty registry is one nobody can write to, and
  // `kit --mint` adds a client and reloads it. A coordinator with no registry
  // takes any token from anyone on the port, which is never what a deployed
  // one should do, and on a dev host costs one `kit --mint` to avoid.
  std::filesystem::path clients = absl::GetFlag(FLAGS_clients).empty()
                                      ? data_dir / "clients.textproto"
                                      : Resolve(absl::GetFlag(FLAGS_clients));
  if (!std::filesystem::exists(clients) &&
      !WriteFile(clients,
                 "# Client registry: one `client { ... }` per participant.\n"
                 "# `arena_tournament kit --mint=<id>` appends here.\n")) {
    LOG(ERROR) << "cannot create " << clients;
    return 1;
  }

  const int grpc_port = absl::GetFlag(FLAGS_grpc_port);
  const int http_port = absl::GetFlag(FLAGS_http_port);
  const std::vector<std::string> server_args = {
      "--problem_config=" + effective.string(),
      "--data_dir=" + data_dir.string(),
      absl::StrCat("--grpc_port=", grpc_port),
      absl::StrCat("--http_port=", http_port),
      "--clients=" + clients.string(),
  };

  struct sigaction action {};
  action.sa_handler = OnStopSignal;
  ::sigaction(SIGINT, &action, nullptr);
  ::sigaction(SIGTERM, &action, nullptr);
  struct sigaction reload {};
  reload.sa_handler = OnReloadSignal;
  ::sigaction(SIGHUP, &reload, nullptr);

  auto server = process::Child::Start(server_bin.string(), server_args,
                                      process::ChildOptions{});
  if (!server) {
    LOG(ERROR) << "cannot start " << server_bin;
    return 1;
  }
  // For `kit --mint` to find the coordinator and have it reload the registry.
  const std::filesystem::path pid_file = data_dir / "problem_server.pid";
  if (!WaitForPort(grpc_port, std::chrono::seconds(60))) {
    LOG(ERROR) << "problem_server did not open port " << grpc_port;
    server->Stop(std::chrono::seconds(5));
    return 1;
  }
  // Written only once the coordinator is actually serving: it is both how
  // `kit --mint` finds it and how `play` knows the tournament in front of it
  // is this one rather than whatever else had the port.
  WriteFile(pid_file, absl::StrCat(server->pid(), "\n"));

  std::vector<process::Child> workers;
  const int worker_count = std::max(1, absl::GetFlag(FLAGS_workers));
  // Each worker's cache volumes are its own: two workers sharing an output
  // base would corrupt it.
  const std::string volume_prefix =
      EnvOr("ARENA_VOLUME_PREFIX", "arena-" + Hostname());
  for (int i = 0; i < worker_count; ++i) {
    process::ChildOptions options;
    options.extra_env = {
        "ARENA_WORK_DIR=" + (data_dir / "work" / std::to_string(i)).string(),
        "ARENA_SLOTS=" + EnvOr("ARENA_SLOTS", "1"),
        absl::StrCat("ARENA_WORKER_ID=local-", i),
        absl::StrCat("ARENA_VOLUME_PREFIX=", volume_prefix, "-", i),
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
      "  arena         localhost:%d   (writes need a token from %s)\n"
      "  state         %s\n"
      "  workers       %d local, building and running in %s\n"
      "\n"
      "A participant's kit (mints a token and reloads the registry):\n"
      "  %s kit --mint=<client_id> --server=<this host>:%d%s\n"
      "\n"
      "%s stops everything.\n\n",
      config->display_name().empty() ? config->problem_id().c_str()
                                     : config->display_name().c_str(),
      http_port, grpc_port, clients.c_str(), data_dir.c_str(), worker_count,
      config->sandbox().image().c_str(),
      runfiles.installed() ? "docker exec <container> arena_tournament"
                           : "bazel run //:kit --",
      grpc_port, runfiles.installed() ? " [--image=REG/kit-<client_id>]" : "",
      runfiles.installed() ? "docker stop" : "Ctrl-C");
  std::fflush(stdout);

  int status = 0;
  while (!g_stop_requested) {
    if (g_reload_requested) {
      g_reload_requested = 0;
      server->Signal(SIGHUP);
    }
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
  std::filesystem::remove(pid_file, ec);
  return status;
}

// ---------------------------------------------------------------------------
// kit
// ---------------------------------------------------------------------------

// The problem's MODULE.bazel, with every relative local_path_override re-rooted
// so it still points where it did from the kit's new location, and with
// game_arena pointed at the copy vendored inside the kit.
//
// Whatever the problem does about the arena -- a git pin, a path on the
// author's machine -- a kit uses the arena beside it. That is the copy the
// participant can read, it is trimmed to what they build against, and it
// needs no network and no checkout of anyone else's.
auto ArenaOverrideBlock() -> std::string {
  return "\n# Written by arena_tournament kit: the arena, trimmed to the "
         "packages\n# this workspace builds against, vendored in ./arena.\n"
         "local_path_override(\n"
         "    module_name = \"game_arena\",\n"
         "    path = \"arena\",\n"
         ")\n";
}

auto WithoutArenaOverride(std::string text) -> std::string {
  static const std::regex kArenaOverride(
      R"re((?:local_path|git|archive|single_version)_override\(\s*module_name\s*=\s*"game_arena"[^)]*\)\n?)re");
  return std::regex_replace(text, kArenaOverride, "");
}

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
      "# arena_cli is not here: it is a program, in .arena/bin and on your\n"
      "# PATH once you have sourced arena.env. Submitting does not need a\n"
      "# build.\n"
      "\n"
      "# The same operations as MCP tools for an agent (see mcp.json).\n"
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
  md << "\n\n`arena_cli` is a program, not a build target: sourcing "
        "`arena.env` puts it on your `PATH` along with the address and the "
        "token, and submitting needs no build.\n";
  md << "\n```sh\n. ./arena.env\n"
        "arena_cli rules\n";
  const auto &kit = config.kit();
  if (!kit.submit_files().empty()) {
    md << "arena_cli submit --name=\"My bot\" --wait   # "
       << absl::StrJoin(kit.submit_files(), ", ") << "\n";
  } else {
    md << "arena_cli submit --name=\"My bot\" --file=<path> --wait\n";
  }
  md << "arena_cli leaderboard\n";
  const std::string rivals =
      kit.source_dir().empty() ? "rivals" : kit.source_dir();
  switch (config.source().visibility()) {
    case proto::SourcePolicy::OWN:
      md << "arena_cli source <your candidate_id>   # your own submissions "
            "only\n";
      break;
    case proto::SourcePolicy::NONE:
      break;
    default:
      md << "arena_cli source <candidate_id>        # pulled into " << rivals
         << "/<candidate_id>/\n";
      break;
  }
  md << "```\n\n";
  md << "`arena.textproto` is what those commands do when you do not say: the "
        "address, what `submit` sends, where `source` puts what it pulls. It "
        "is yours to edit -- it steers your tools, and the tournament still "
        "decides what it accepts.\n";
  switch (config.source().visibility()) {
    case proto::SourcePolicy::OWN:
      md << "\nThis tournament serves you only your own submissions' source; "
            "a rival's is refused.\n";
      break;
    case proto::SourcePolicy::NONE:
      md << "\nThis tournament serves no submission's source, not even your "
            "own. What is in this kit is what you have.\n";
      break;
    default:
      md << "\nEvery candidate's source is readable here: `source` is how you "
            "learn from what is beating you.\n";
      break;
  }
  md << "\nFor an agent, `mcp.json` registers the same operations as MCP "
        "tools (`arena_rules`, `arena_submit`, `arena_job`, "
        "`arena_leaderboard`, `arena_source`, `arena_evaluate`); "
        "`arena_submit` takes file paths relative to this directory.\n";
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

// The arena a kit builds against: the packages a participant's workspace
// actually compiles against, and nothing else.
//
// A kit is a bazel workspace whose files `#include` the arena's game session
// and link its play loop, so it needs @game_arena -- but it needs the game
// side of it. The coordinator, the sandbox, the fleet, the tools and the
// arena's own problems are not a participant's business: they are noise in
// their editor, noise in their build graph, and a map of the machinery that
// decides their score. So the kit gets a copy of the six packages its targets
// reach and nothing else, vendored beside their files:
//
//   bazel query 'deps(<the kit's targets>)' inside a kit stays within these.
//
// Each is closed under dependency (only the others and external modules), so
// this list is checkable rather than hopeful: //game_arena:kit_surface_test
// fails when something a kit builds reaches outside them.
constexpr std::array<std::string_view, 7> kKitSurfaceDirs = {
    "game_arena/proto", "game_arena/referee",   "game_arena/client",
    "game_arena/cli",   "game_arena/standings", "game_arena/common/kv_options",
    "mcp_servers",
};

// Root files of the module itself: what MODULE.bazel needs to be usable as a
// local_path_override, and the bazel it was written for.
constexpr std::array<std::string_view, 4> kKitSurfaceFiles = {
    "MODULE.bazel", ".bazelversion", "protobuf_python_dist_build.patch",
    "protobuf_python_dist_bzl.patch"};

// A runfiles tree holds a package's sources (symlinks into the checkout) and,
// beside them, anything built from it that something depends on -- arena_cli's
// own binary, for one, which is a symlink into bazel-out. A kit vendors the
// sources; the binary it gets is the builtin, installed once.
auto IsBuildOutput(const std::filesystem::path &path) -> bool {
  std::error_code ec;
  const std::filesystem::path real =
      std::filesystem::weakly_canonical(path, ec);
  return !ec && real.string().find("/bazel-out/") != std::string::npos;
}

// Copies that surface into <kit>/arena. Under `bazel run` it comes from this
// tool's runfiles; installed (the tournament image) from the module copy the
// image carries.
auto InstallArenaSurface(const ArenaRunfiles &runfiles,
                         const std::filesystem::path &out) -> bool {
  const std::filesystem::path arena = out / "arena";
  std::error_code ec;
  std::filesystem::remove_all(arena, ec);
  std::filesystem::create_directories(arena, ec);
  for (const std::string_view dir : kKitSurfaceDirs) {
    const std::filesystem::path from = runfiles.LocateSource(std::string(dir));
    if (from.empty() || !std::filesystem::is_directory(from)) {
      LOG(ERROR) << "cannot find the arena's " << dir
                 << " to vendor into the kit. Run this from a checkout of "
                    "the arena, or point --arena_override at one";
      return false;
    }
    const std::filesystem::path to = arena / std::string(dir);
    std::filesystem::create_directories(to, ec);
    for (const auto &entry :
         std::filesystem::recursive_directory_iterator(from, ec)) {
      if (!entry.is_regular_file() || IsBuildOutput(entry.path())) {
        continue;
      }
      // Lexical: std::filesystem::relative() resolves symlinks, and every
      // entry of a runfiles tree is one pointing back at the checkout, so it
      // would answer with a path out of the kit and into the arena's own
      // sources.
      const std::filesystem::path target =
          to / entry.path().lexically_relative(from);
      std::filesystem::create_directories(target.parent_path(), ec);
      std::filesystem::copy_file(
          entry.path(), target,
          std::filesystem::copy_options::overwrite_existing, ec);
      if (ec) {
        LOG(ERROR) << "cannot copy " << entry.path().string()
                   << " into the kit: " << ec.message();
        return false;
      }
    }
  }
  for (const std::string_view file : kKitSurfaceFiles) {
    const std::filesystem::path from = runfiles.LocateSource(std::string(file));
    if (from.empty() || !std::filesystem::is_regular_file(from)) {
      LOG(ERROR) << "cannot find the arena's " << file << " to vendor";
      return false;
    }
    std::filesystem::copy_file(
        from, arena / std::string(file),
        std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) {
      LOG(ERROR) << "cannot copy " << file << ": " << ec.message();
      return false;
    }
  }
  // The module's root package: the two patches MODULE.bazel names are labels,
  // so the package has to exist. The arena's own root BUILD is not copied --
  // it exports files this surface does not have.
  return WriteFile(
      arena / "BUILD",
      "# The vendored arena's root package: the patches MODULE.bazel names.\n"
      "exports_files([\n"
      "    \"protobuf_python_dist_build.patch\",\n"
      "    \"protobuf_python_dist_bzl.patch\",\n"
      "])\n");
}

// The kit's builtins: the tools a participant runs, installed as programs
// rather than reachable as bazel targets.
//
// arena_cli is how a participant enters the tournament, and needing a
// toolchain and a resolved module graph to submit is both a delay and a
// distraction -- the arena's build is not their problem. The binary is copied
// out of this tool's own runfiles, so it is the arena they are entering.
//
// A kit image builds its own copy instead (see the Dockerfile): this one was
// linked against the host's libraries, and the image is not this host.
auto InstallBuiltins(const ArenaRunfiles &runfiles,
                     const std::filesystem::path &out) -> bool {
  const std::filesystem::path cli = runfiles.Locate("game_arena/cli/arena_cli");
  if (cli.empty()) {
    LOG(ERROR) << "cannot find arena_cli to install into the kit";
    return false;
  }
  const std::filesystem::path bin = out / ".arena" / "bin";
  std::error_code ec;
  std::filesystem::create_directories(bin, ec);
  const std::filesystem::path target = bin / "arena_cli";
  // `play` rewrites the kit on every run and this is a large binary on a
  // possibly slow disk; only copy when it is not already the one we have.
  if (std::filesystem::exists(target) &&
      std::filesystem::file_size(target, ec) ==
          std::filesystem::file_size(cli, ec) &&
      !ec) {
    return true;
  }
  std::filesystem::remove(target, ec);
  std::filesystem::copy_file(
      cli, target, std::filesystem::copy_options::overwrite_existing, ec);
  if (ec) {
    LOG(ERROR) << "cannot install arena_cli into " << bin << ": "
               << ec.message();
    return false;
  }
  std::filesystem::permissions(target,
                               std::filesystem::perms::owner_all |
                                   std::filesystem::perms::group_read |
                                   std::filesystem::perms::group_exec |
                                   std::filesystem::perms::others_read |
                                   std::filesystem::perms::others_exec,
                               ec);
  return true;
}

// The kit's own config, as the participant finds it: what arena_cli does when
// they do not say. Everything in it is theirs to change -- the coordinator
// enforces the problem's policy on what actually arrives.
auto KitConfigText(const proto::ProblemConfig &config,
                   const std::string &server, const std::string &http,
                   const std::string &client_id) -> std::string {
  proto::KitConfig kit;
  kit.set_problem_id(config.problem_id());
  kit.set_server(server);
  kit.set_http(http);
  kit.set_client_id(client_id);
  for (const std::string &file : config.kit().submit_files()) {
    kit.add_submit_files(file);
  }
  kit.set_source_dir(
      config.kit().source_dir().empty() ? "rivals" : config.kit().source_dir());
  std::string text;
  google::protobuf::TextFormat::PrintToString(kit, &text);
  return absl::StrCat(
      "# The kit's config: what arena_cli does when you do not tell it.\n"
      "# Yours to edit -- it steers your tools and nothing else. The\n"
      "# tournament enforces its own rules on what arrives, so widening\n"
      "# anything here changes what you send, never what is accepted.\n"
      "#\n"
      "# Schema: tournament_arena.proto.KitConfig.\n",
      text);
}

auto RunKit(const ArenaRunfiles &runfiles) -> int {
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
    // The coordinator `up` started on that registry, if it is running here:
    // tell it, so the token works now rather than after a restart.
    const auto pid_text =
        ReadFile(clients.parent_path() / "problem_server.pid");
    const pid_t pid = pid_text ? std::atoi(pid_text->c_str()) : 0;
    if (pid > 0 && ::kill(pid, SIGHUP) == 0) {
      std::printf("The coordinator (pid %d) is reloading it.\n", pid);
    } else {
      std::printf(
          "A coordinator already running on that registry needs a SIGHUP to "
          "see it (`docker kill -s HUP <container>` for a deployed one).\n");
    }
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
  if (!InstallArenaSurface(runfiles, out)) {
    return 1;
  }
  WriteFile(
      out / "MODULE.bazel",
      absl::StrCat(WithoutArenaOverride(RerootedModuleFile(*module, root)),
                   ArenaOverrideBlock()));
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

  // What the problem tells a participant to submit has to be in what it gave
  // them. Getting this wrong produces a kit whose one documented command
  // fails, and the person who finds out is the participant.
  for (const std::string &file : config->kit().submit_files()) {
    if (!std::filesystem::exists(out / file)) {
      LOG(ERROR) << "kit.submit_files names \"" << file
                 << "\", which is not in the kit. Add it to the macro's "
                    "kit_files, or name a path that is there";
      return 1;
    }
  }

  const std::string server = absl::GetFlag(FLAGS_server);
  const std::string http = absl::GetFlag(FLAGS_http);
  const std::string registry = absl::GetFlag(FLAGS_registry).empty()
                                   ? EnvOr("ARENA_KIT_REGISTRY", "")
                                   : absl::GetFlag(FLAGS_registry);

  WriteFile(out / "BUILD", KitBuildFile(registry));
  WriteFile(out / "ARENA.md",
            KitReadme(*config, server, http, client_id, !token.empty(), files));

  // Sourced, not run, and it finds itself: a kit that is moved or handed on
  // still points its tools at its own directory.
  std::string env = absl::StrCat(
      "# . ./arena.env -- your tournament, in this shell.\n"
      "ARENA_KIT=\"$(cd \"$(dirname \"${BASH_SOURCE[0]:-$0}\")\" && pwd)\"\n"
      "export ARENA_KIT\n"
      "export PATH=\"$ARENA_KIT/.arena/bin:$PATH\"\n"
      "export ARENA_SERVER=",
      server, "\n");
  if (!token.empty()) {
    absl::StrAppend(&env, "export ARENA_TOKEN=", token, "\n");
  }
  if (!client_id.empty()) {
    absl::StrAppend(&env, "export ARENA_MCP_AUTHOR=", client_id, "\n");
  }
  WriteFile(out / "arena.env", env);
  WriteFile(out / "arena.textproto",
            KitConfigText(*config, server, http, client_id));
  if (!InstallBuiltins(runfiles, out)) {
    return 1;
  }

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
  absl::StrAppend(&mcp, ",\n        \"ARENA_KIT\": \"",
                  JsonEscape(out.string()), "\"");
  absl::StrAppend(&mcp, "\n      }\n    }\n  }\n}\n");
  WriteFile(out / "mcp.json", mcp);

  // bazel-* are symlinks the priming build leaves behind; .bazelrc.local
  // names this host's paths; .arena/cache is a bazel disk cache, which an
  // image builds for itself and a git history should never carry. The
  // builtins are this host's binaries, so an image rebuilds those too.
  // The vendored arena is a module, not part of this workspace: without this
  // `bazel build //...` here would try to load its packages as the kit's own
  // and fail on labels that only resolve inside it.
  WriteFile(out / ".bazelignore",
            "# The arena is a bazel module of its own (MODULE.bazel points at\n"
            "# it); //... is your workspace, not it.\narena\n");
  WriteFile(out / ".gitignore",
            "bazel-*\n.bazelrc.local\n.arena/cache/\n.arena/bin/\n");
  WriteFile(out / ".dockerignore",
            "bazel-*\n.bazelrc.local\n.arena/cache/\n.arena/bin/\n.git\n");
  // This host's settings, in the file the problem's own .bazelrc is told to
  // try-import: an absolute path, because bazel does not expand %workspace%
  // inside a flag's value, and uncommitted, because it names this machine.
  const std::string arena_override = absl::GetFlag(FLAGS_arena_override);
  const std::filesystem::path cache = out / ".arena" / "cache";
  std::string local =
      "# Written by arena_tournament kit. This host's paths; not committed,\n"
      "# and not copied into an image of the kit.\n";
  if (absl::GetFlag(FLAGS_prime_cache)) {
    // Bounded: this lives inside someone's working directory, and a cache
    // that only grows is a surprise they find out about from `df`.
    absl::StrAppend(&local, "build --disk_cache=", cache.string(),
                    "\ncommon --experimental_disk_cache_gc_max_size=",
                    absl::GetFlag(FLAGS_cache_limit), "\n");
  }
  if (!arena_override.empty()) {
    absl::StrAppend(&local, "common --override_module=game_arena=",
                    Resolve(arena_override).string(), "\n");
  }
  // Only the lines this tool owns are rewritten. `play` writes the kit again
  // on every run, and a participant who put their own cache or their own
  // flags in here should not lose them to that.
  if (const auto existing = ReadFile(out / ".bazelrc.local")) {
    for (std::string_view line : absl::StrSplit(*existing, '\n')) {
      if (line.empty() || line.front() == '#' ||
          line.find("--disk_cache=" + cache.string()) !=
              std::string_view::npos ||
          line.find("--override_module=game_arena=") !=
              std::string_view::npos) {
        continue;
      }
      absl::StrAppend(&local, line, "\n");
    }
  }
  WriteFile(out / ".bazelrc.local", local);
  const auto rc = ReadFile(out / ".bazelrc");
  if (!rc || rc->find(".bazelrc.local") == std::string::npos) {
    WriteFile(out / ".bazelrc",
              absl::StrCat(rc.value_or(""),
                           "\ntry-import %workspace%/.bazelrc.local\n"));
  }

  std::printf("Kit for %s written to %s:\n", config->problem_id().c_str(),
              out.c_str());
  std::printf("  ARENA.md         the rules, and how to submit\n");
  std::printf(
      "  arena.env        . ./arena.env puts arena_cli on your PATH%s\n",
      token.empty() ? "" : " with your token");
  std::printf("  arena.textproto  what arena_cli does by default; yours\n");
  std::printf("  mcp.json         the same operations as MCP tools\n");
  std::printf("  %zu file(s) from the problem\n", files.size());
  if (!token.empty()) {
    std::printf(
        "\nThe token is inside the kit; hand the directory to one "
        "participant only.\n");
  }

  if (absl::GetFlag(FLAGS_prime_cache)) {
    std::printf(
        "\nBuilding the kit once, into %s (the first time takes a while; "
        "--prime_cache=false skips it)...\n",
        cache.c_str());
    std::fflush(stdout);
    const int code = RunInherit("bazel", {"build", "//..."}, out);
    if (code != 0) {
      LOG(ERROR) << "the kit does not build (exit " << code
                 << "); kit_files is probably missing a BUILD file or a "
                    "source one of them depends on";
      return 1;
    }
    std::printf("Kit builds, and its cache is warm.\n");
  }

  const std::string image = absl::GetFlag(FLAGS_image);
  if (image.empty()) {
    return 0;
  }
  const std::filesystem::path dockerfile =
      runfiles.Locate("game_arena/image/Dockerfile");
  if (dockerfile.empty()) {
    return 1;
  }
  if (server.rfind("localhost", 0) == 0 || server.rfind("127.", 0) == 0) {
    LOG(WARNING) << "--server=" << server
                 << " is baked into the image, and inside a container that "
                    "is the container itself; pass --server=<host>:<port> "
                    "as the coordinator is reached from where the kit runs, "
                    "or override with `docker run -e ARENA_SERVER=...`";
  }
  // The problem's own layer, if it has one: built first, from the problem's
  // repo, and passed in to replace the empty kit_extras stage. Its Dockerfile
  // starts `FROM kit_base`, which is the arena's toolchain stage, built here
  // so it has something to start from.
  std::vector<std::string> contexts;
  if (!config->kit().dockerfile().empty()) {
    const std::filesystem::path problem_dockerfile =
        root / config->kit().dockerfile();
    if (!std::filesystem::is_regular_file(problem_dockerfile)) {
      LOG(ERROR) << "kit.dockerfile names " << problem_dockerfile.string()
                 << ", which is not a file";
      return 1;
    }
    const std::string base = image + "-kit-base";
    const std::string extras = image + "-kit-extras";
    std::printf("\nBuilding %s, this problem's own layer of the kit...\n",
                extras.c_str());
    std::fflush(stdout);
    if (!DockerBuild(dockerfile, "kit_base", base, out, {}, {},
                     /*push=*/false, /*with_arena_context=*/false) ||
        !DockerBuild(problem_dockerfile, "", extras, root, {},
                     {"kit_base=docker-image://" + base}, /*push=*/false,
                     /*with_arena_context=*/false)) {
      return 1;
    }
    contexts.push_back("kit_extras=docker-image://" + extras);
  }

  std::printf(
      "\nBuilding %s from the kit (a full build of it; the first "
      "time takes a while)...\n",
      image.c_str());
  std::fflush(stdout);
  // No game_arena build context: a kit image builds against the arena the kit
  // carries, so a copy of the host's checkout would be a second one nothing
  // reads.
  if (!DockerBuild(dockerfile, "kit", image, out,
                   {"ARENA_SERVER=" + server, "ARENA_HTTP=" + http,
                    "ARENA_TOKEN=" + token, "ARENA_CLIENT_ID=" + client_id,
                    absl::StrCat("ARENA_KIT_VENDOR=",
                                 config->kit().allow_network_builds() ? 0 : 1)},
                   contexts, absl::GetFlag(FLAGS_push),
                   /*with_arena_context=*/false)) {
    return 1;
  }
  std::printf(
      "\nBuilt %s%s. It is one participant's environment, token "
      "included; run it wherever they work:\n\n",
      image.c_str(), absl::GetFlag(FLAGS_push) ? " and pushed it" : "");
  std::printf(
      "  docker run -it %s                          # a shell in "
      "/kit, everything built\n",
      image.c_str());
  std::printf(
      "  docker run -it %s arena_cli leaderboard      # the builtin, on "
      "PATH\n",
      image.c_str());
  std::printf(
      "  docker run -i %s bazel run //:mcp_server   # the MCP "
      "server on stdio, for an agent\n",
      image.c_str());
  std::printf(
      "  docker run -e ARENA_SERVER=<host:port> ...   # the same kit "
      "against another coordinator\n");
  return 0;
}

// ---------------------------------------------------------------------------
// image
// ---------------------------------------------------------------------------

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
      runfiles.Locate("game_arena/image/Dockerfile");
  if (dockerfile.empty()) {
    return 1;
  }
  const std::filesystem::path root = WorkspaceRoot();
  if (!std::filesystem::exists(root / "MODULE.bazel")) {
    LOG(ERROR) << "no MODULE.bazel at " << root
               << "; the image is built from the problem's workspace";
    return 1;
  }

  if (!DockerBuild(dockerfile, "sandbox", tag, root, {})) {
    return 1;
  }

  std::printf("\nBuilt %s. Smoke test it offline:\n\n", tag.c_str());
  std::printf("  docker run --rm --network=none -v %s:/src:ro -w /src %s \\\n",
              root.c_str(), tag.c_str());
  std::printf("      bazel --output_base=/tmp/ob build %s\n",
              absl::StrJoin(config->build().targets(), " ").c_str());
  return 0;
}

// ---------------------------------------------------------------------------
// play
// ---------------------------------------------------------------------------

// The value of KEY=... in a shell-style env file (`export KEY=value` lines).
auto EnvFileValue(const std::filesystem::path &file,
                  std::string_view key) -> std::string {
  const auto text = ReadFile(file);
  if (!text) {
    return "";
  }
  for (std::string_view line : absl::StrSplit(*text, '\n')) {
    line = absl::StripPrefix(line, "export ");
    if (absl::ConsumePrefix(&line, key) && absl::ConsumePrefix(&line, "=")) {
      return std::string(absl::StripAsciiWhitespace(line));
    }
  }
  return "";
}

auto TailOf(const std::filesystem::path &file, int lines) -> std::string {
  const auto text = ReadFile(file);
  if (!text) {
    return "";
  }
  std::size_t pos = text->size();
  for (int i = 0; i <= lines && pos != std::string::npos && pos > 0; ++i) {
    pos = text->rfind('\n', pos - 1);
  }
  return text->substr(pos == std::string::npos ? 0 : pos + 1);
}

// `up` and `kit` are run as this binary's own subprocesses rather than called:
// they already have the flags, the checks and the messages, and the shell is
// the only thing new here.
auto RunPlay() -> int {
  std::filesystem::path config_path;
  const auto config = LoadConfig(&config_path);
  if (!config) {
    return 1;
  }
  std::error_code ec;
  const std::filesystem::path self =
      std::filesystem::read_symlink("/proc/self/exe", ec);
  if (ec) {
    LOG(ERROR) << "cannot find myself: " << ec.message();
    return 1;
  }
  const std::filesystem::path data_dir =
      absl::GetFlag(FLAGS_data_dir).empty()
          ? StateDir(config->problem_id())
          : Resolve(absl::GetFlag(FLAGS_data_dir));
  const std::filesystem::path clients =
      absl::GetFlag(FLAGS_clients).empty()
          ? data_dir / "clients.textproto"
          : Resolve(absl::GetFlag(FLAGS_clients));
  const std::string client_id = absl::GetFlag(FLAGS_mint).empty()
                                    ? EnvOr("USER", "player")
                                    : absl::GetFlag(FLAGS_mint);
  const std::filesystem::path kit_dir = absl::GetFlag(FLAGS_out).empty()
                                            ? data_dir / "kits" / client_id
                                            : Resolve(absl::GetFlag(FLAGS_out));
  const int grpc_port = absl::GetFlag(FLAGS_grpc_port);
  const int http_port = absl::GetFlag(FLAGS_http_port);

  // Your token, if a kit of yours is here already: minting again would be
  // refused (one client id, one entry) and would orphan the old one anyway.
  std::string token = EnvFileValue(kit_dir / "arena.env", "ARENA_TOKEN");
  if (token.empty()) {
    const auto registry = ReadFile(clients);
    if (registry && registry->find(absl::StrCat("client_id: \"", client_id,
                                                "\"")) != std::string::npos) {
      LOG(ERROR) << "'" << client_id << "' is in " << clients
                 << " but there is no kit at " << kit_dir
                 << " holding its token. Pass --mint=<another id>, or remove "
                    "that client's block from the registry";
      return 1;
    }
  }

  // The tournament, in the background, its logs in a file: a shell over a
  // stream of worker logs is no shell.
  const std::filesystem::path log = data_dir / "logs" / "tournament.log";
  std::filesystem::create_directories(log.parent_path(), ec);
  std::vector<std::string> up_args = {
      "up",
      "--problem_config=" + config_path.string(),
      "--data_dir=" + data_dir.string(),
      "--clients=" + clients.string(),
      absl::StrCat("--grpc_port=", grpc_port),
      absl::StrCat("--http_port=", http_port),
      absl::StrCat("--workers=", absl::GetFlag(FLAGS_workers)),
  };
  // The coordinator writes this once it is serving, so waiting for it means
  // waiting for *our* tournament. Waiting for the port would not: something
  // else on this host may already hold it, and then the kit would be pointed
  // at a stranger's arena while ours failed to bind behind our back.
  const std::filesystem::path pid_file = data_dir / "problem_server.pid";
  std::filesystem::remove(pid_file, ec);
  process::ChildOptions up_options;
  up_options.stdout_path = log;
  up_options.stderr_path = log;
  auto up = process::Child::Start(self.string(), up_args, up_options);
  if (!up) {
    LOG(ERROR) << "cannot start " << self;
    return 1;
  }
  std::printf(
      "Starting the tournament (log: %s).\n"
      "The first run builds the problem's sandbox image, which takes a "
      "while.\n",
      log.c_str());
  std::fflush(stdout);
  while (!std::filesystem::exists(pid_file)) {
    if (const auto code = up->Poll()) {
      LOG(ERROR) << "the tournament did not start (exit " << *code << "):\n"
                 << TailOf(log, 15);
      return 1;
    }
    if (g_stop_requested) {
      up->Stop(std::chrono::seconds(10));
      return 1;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }

  // Your kit, rewritten each time so it has the current kit_files.
  std::vector<std::string> kit_args = {
      "kit",
      "--problem_config=" + config_path.string(),
      "--out=" + kit_dir.string(),
      "--clients=" + clients.string(),
      absl::StrCat("--server=localhost:", grpc_port),
      absl::StrCat("--http=localhost:", http_port),
      "--force",
      token.empty() ? "--mint=" + client_id : "--token=" + token,
  };
  if (!absl::GetFlag(FLAGS_arena_override).empty()) {
    kit_args.push_back("--arena_override=" +
                       absl::GetFlag(FLAGS_arena_override));
  }
  if (RunInherit(self.string(), kit_args, WorkspaceRoot()) != 0) {
    up->Stop(std::chrono::seconds(10));
    return 1;
  }
  if (token.empty()) {
    token = EnvFileValue(kit_dir / "arena.env", "ARENA_TOKEN");
  }

  struct sigaction action {};
  action.sa_handler = OnStopSignal;
  ::sigaction(SIGINT, &action, nullptr);
  ::sigaction(SIGTERM, &action, nullptr);

  std::printf(
      "\n"
      "You are '%s' in your kit at %s.\n"
      "  leaderboard   http://localhost:%d/\n"
      "  arena_cli is on your PATH, with ARENA_SERVER and ARENA_TOKEN set;\n"
      "  see ARENA.md. For example:\n"
      "    arena_cli rules\n"
      "    arena_cli submit --name=try --wait\n"
      "    arena_cli leaderboard\n"
      "  tournament log: %s\n"
      "Leaving this shell stops the tournament.\n\n",
      client_id.c_str(), kit_dir.c_str(), http_port, log.c_str());
  std::fflush(stdout);

  const std::string shell = absl::GetFlag(FLAGS_shell).empty()
                                ? EnvOr("SHELL", "bash")
                                : absl::GetFlag(FLAGS_shell);
  process::ChildOptions shell_options;
  shell_options.cwd = kit_dir;
  shell_options.extra_env = {
      absl::StrCat("ARENA_SERVER=localhost:", grpc_port),
      "ARENA_TOKEN=" + token,
      "ARENA_MCP_AUTHOR=" + client_id,
      "ARENA_KIT=" + kit_dir.string(),
      // The kit's builtins, ahead of whatever else is called arena_cli.
      absl::StrCat("PATH=", (kit_dir / ".arena" / "bin").string(), ":",
                   EnvOr("PATH", "/usr/local/bin:/usr/bin:/bin")),
  };
  auto sh = process::Child::Start(shell, {}, shell_options);
  if (!sh) {
    LOG(ERROR) << "cannot start " << shell;
    up->Stop(std::chrono::seconds(10));
    return 1;
  }
  // The shell gets the terminal: Ctrl-C then reaches its jobs, not us. A
  // background process group writing to the terminal is fine; changing its
  // owner from one is not, hence SIGTTOU ignored for the hand-back below.
  const bool tty = ::isatty(STDIN_FILENO) != 0;
  if (tty) {
    ::signal(SIGTTOU, SIG_IGN);
    ::tcsetpgrp(STDIN_FILENO, sh->pid());
  }

  bool tournament_died = false;
  while (!g_stop_requested && !sh->Poll()) {
    if (const auto code = up->Poll(); code && !tournament_died) {
      tournament_died = true;
      std::fprintf(stderr,
                   "\n[arena] the tournament exited with %d; see %s. "
                   "`exit` to leave.\n",
                   *code, log.c_str());
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }
  if (g_stop_requested) {
    sh->Stop(std::chrono::seconds(2));
  }
  if (tty) {
    ::tcsetpgrp(STDIN_FILENO, ::getpgrp());
  }
  std::printf("Stopping the tournament...\n");
  std::fflush(stdout);
  const int status = up->Stop(std::chrono::seconds(20));
  return tournament_died ? 1 : (status == 0 ? 0 : 1);
}

void PrintUsage() {
  std::fprintf(stderr,
               "usage: arena_tournament <up|kit|check|image|play> "
               "--problem_config=<path> [flags]\n"
               "  up      run a coordinator and local workers (--image)\n"
               "  play    up in the background, a kit for you, a shell in it\n"
               "  kit     write a participant's workspace (--out, --mint, "
               "--image)\n"
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
    return RunKit(runfiles);
  }
  if (command == "check") {
    return RunCheck();
  }
  if (command == "image") {
    return RunImage(runfiles);
  }
  if (command == "play") {
    return RunPlay();
  }
  PrintUsage();
  return 2;
}
