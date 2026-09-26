// Standing a tournament up from a problem repository, and handing participants
// a kit to enter it.
/*
bazel run //:tournament
bazel run //:play                            # all of it, and a shell in your
kit bazel run //:kit -- --out=/srv/kits/alice --server=arena:50051 --mint=alice
bazel build //:kit_image
bazel run //:kit_image_issue -- --mint=bob --server=arena:50051 \
    --image=registry/kit-bob --push
bazel build //:sandbox_image
bazel test //:config_test

Without the macro, from a problem repo:
bazel run @game_arena//game_arena/tools:arena_tournament -- \
    up --problem_config=problem.textproto
*/
//
// Four subcommands, one problem config. (The sandbox image is not one of them:
// it is a build output and nothing else, the macro's sandbox_image.)
//
//   up      the coordinator, on this checkout, and nothing else: it builds
//           and runs nothing. A worker is a process of its own
//           (sandbox_worker), started by whoever wants the capacity.
//   kit     a participant's workspace: the files the problem names, the
//           arena's CLI and MCP server reachable through @game_arena, a README
//           from the config, and a freshly minted token. The kit as an
//           image is a build output (the macro's kit_image: this subcommand
//           run in an action, stacked on a base by rules_oci); with --image,
//           run as the macro's kit_image_issue, this adds to that image what
//           a build cannot -- the dependencies vendored and the cache
//           primed, and the token -- with no docker involved.
//   check   the config parses and is consistent with the tree. What
//           :config_test runs.
//   play    up, in the background with its logs in a file, a kit minted for
//           you, and a shell in it with the arena's address and your token in
//           the environment. Leaving the shell stops everything. The dev loop.
//
// The tool knows what a problem *config* is, never what a problem is made of.
// Every label, file and name here arrives from the caller.

#include <arpa/inet.h>
#include <google/protobuf/text_format.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
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
#include "game_arena/common/kv_options/kv_options.h"
#include "game_arena/common/process/process.h"
#include "game_arena/proto/kit.pb.h"
#include "game_arena/server/client_registry.h"
#include "game_arena/server/problem_config.h"
#include "rules_cc/cc/runfiles/runfiles.h"

ABSL_FLAG(std::string, problem_config, "",
          "The problem's .textproto (required). "
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
ABSL_FLAG(std::string, clients, "",
          "up/kit: the client registry. up passes it to the coordinator, "
          "creating it empty if it does not exist; kit --mint appends to it, "
          "and the coordinator reads it on the token's first use. Default: "
          "<data_dir>/clients.textproto");

// kit
ABSL_FLAG(std::string, out, "",
          "kit: directory to write the kit into (created). Default: "
          "<data_dir>/kits/<client_id>; for --image, <data_dir>/images/kit, "
          "which keeps what was vendored and primed between images");
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
          "kit: label of the problem's GameRegistry() library, for the "
          "referee in the kit. Supplied by the arena_problem macro; default "
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
ABSL_FLAG(std::string, prime_bazelrc, "",
          "kit: a bazelrc for the priming build and vendoring only, e.g. a "
          "remote cache that executes locally. Not copied into the kit");
ABSL_FLAG(bool, force, false, "kit: write into a non-empty --out");
ABSL_FLAG(std::string, kit_path, "",
          "kit: where the kit will be used from, when that is not --out: the "
          "path baked into mcp.json and .bazelrc.local. The kit inside an "
          "image is written somewhere else and lives at /kit");
ABSL_FLAG(std::string, image, "",
          "kit: derive an image with this tag from the kit image bazel built "
          "(--kit_base): the kit's dependencies vendored and its build cache "
          "primed, and the token when one is minted or given. No docker "
          "involved; run it as //:kit_image_issue, the target that carries "
          "the built image. --push pushes it");
ABSL_FLAG(std::string, kit_base, "",
          "kit --image: the OCI layout that is added to -- the kit image "
          "`bazel build //:kit_image` makes. Supplied by the arena_problem "
          "macro's kit_image_issue target; default $ARENA_KIT_BASE");
ABSL_FLAG(std::string, regctl, "",
          "--image: the regctl binary that adds the layers and "
          "pushes. Supplied with the base; default $ARENA_REGCTL");

// play
ABSL_FLAG(std::string, shell, "",
          "play: the shell to drop into the kit. Default: $SHELL, else bash");

ABSL_FLAG(bool, push, false,
          "--image: push the result to its registry, with the logins "
          "docker keeps. Without it the image is loaded into the local "
          "daemon, or left as an archive when there is none");
ABSL_FLAG(std::string, docker, "docker",
          "up: the docker binary the sandbox image is looked for with. "
          "image, --image: the one an unpushed image is loaded with");

namespace {

namespace proto = tournament_arena::proto;
using rules_cc::cc::runfiles::Runfiles;

volatile std::sig_atomic_t g_stop_requested = 0;

extern "C" void OnStopSignal(int /*signum*/) { g_stop_requested = 1; }

std::string EnvOr(const char *name, std::string fallback) {
  const char *value = std::getenv(name);
  return value != nullptr && *value != '\0' ? std::string(value) : fallback;
}

// Under `bazel run`, the checkout the target was run from; otherwise the
// current directory.
std::filesystem::path WorkspaceRoot() {
  const std::string root = EnvOr("BUILD_WORKSPACE_DIRECTORY", "");
  return root.empty() ? std::filesystem::current_path()
                      : std::filesystem::path(root);
}

auto Resolve(const std::string &path) -> std::filesystem::path {
  const std::filesystem::path p(path);
  return p.is_absolute() ? p : WorkspaceRoot() / p;
}

std::filesystem::path StateDir(std::string_view problem_id) {
  const std::filesystem::path base =
      EnvOr("ARENA_STATE_DIR", EnvOr("HOME", "/tmp") + "/.arena");
  return base / std::string(problem_id);
}

std::optional<std::string> ReadFile(const std::filesystem::path &path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return std::nullopt;
  }
  return std::string((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());
}

bool WriteFile(const std::filesystem::path &path, std::string_view text) {
  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out << text;
  return static_cast<bool>(out);
}

// Runs |executable| with the caller's stdout/stderr and waits. -1 when it
// could not be started.
int RunInherit(const std::string &executable,
               const std::vector<std::string> &arguments,
               const std::filesystem::path &cwd) {
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
std::optional<std::string> Capture(const std::string &executable,
                                   const std::vector<std::string> &arguments,
                                   const std::filesystem::path &cwd) {
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

bool WaitForPort(int port, std::chrono::seconds timeout) {
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

std::optional<proto::ProblemConfig> LoadConfig(
    std::filesystem::path *config_path) {
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

// The arena's files, from runfiles.
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
    const std::filesystem::path found = LocateSource(path);
    if (found.empty()) {
      LOG(ERROR) << "cannot find " << path
                 << " in runfiles; is this running under bazel run?";
    }
    return found;
  }

  // The same lookup, but empty when it is not there is not an error by
  // itself -- only the kit's vendored arena needs sources.
  auto LocateSource(const std::string &path) const -> std::filesystem::path {
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
    return {};
  }

 private:
  std::unique_ptr<Runfiles> runfiles_;
};

// ---------------------------------------------------------------------------
// check
// ---------------------------------------------------------------------------

bool CheckConfig(const proto::ProblemConfig &config,
                 const std::filesystem::path &config_path, std::string *error) {
  // The problem's tree is where its config is, and only there to check under
  // `bazel run`: a test has the config as a lone data file.
  const std::string tree = EnvOr("BUILD_WORKSPACE_DIRECTORY", "").empty()
                               ? ""
                               : config_path.parent_path().string();
  const auto in_tree = [&tree](const std::string &path) {
    return std::filesystem::exists(std::filesystem::path(tree) / path);
  };
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
    if (!tree.empty() && !in_tree(submit_dir)) {
      *error = absl::StrCat("submission.files_submit_dir \"", submit_dir,
                            "\" does not exist under ", tree);
      return false;
    }
  }
  // A sandbox builds with no network, so the module graph has to be settled
  // in the tree its image carries: without a lockfile the build re-resolves
  // against the registry and fails.
  if (!config.sandbox().allow_build_network() && !tree.empty() &&
      !in_tree("MODULE.bazel.lock")) {
    *error = absl::StrCat(
        "no MODULE.bazel.lock in ", tree,
        "; the sandbox builds without a network and needs it. Run a build");
    return false;
  }
  return true;
}

int RunCheck() {
  std::filesystem::path config_path;
  const auto config = LoadConfig(&config_path);
  if (!config) {
    return 1;
  }
  std::string error;
  if (!CheckConfig(*config, config_path, &error)) {
    LOG(ERROR) << config_path.string() << ": " << error;
    return 1;
  }
  std::printf("OK: %s (%s, %s)\n", config_path.c_str(),
              config->problem_id().c_str(),
              config->has_match() ? "match" : "graded");
  return 0;
}

// ---------------------------------------------------------------------------
// images
// ---------------------------------------------------------------------------

// Every image here is a build output with more added to it outside the build.
// `bazel build` makes what a build can hold -- a base pulled by digest, and
// files stacked on it by rules_oci -- and this tool adds, as further layers,
// what a build cannot: a token (a secret, which a remote cache would keep)
// and the result of running bazel (a kit's vendored dependencies and primed
// cache). The sandbox image has neither, and is a build output whole. It
// does that with the regctl rules_oci built the image with,
// on the OCI layout itself, so no step of making an image runs a container or
// needs a daemon.
//
// The built image and regctl arrive from the macro target that does the
// adding, as paths into runfiles relative to where `bazel run` started this.
struct ImageTools {
  std::filesystem::path base;
  std::filesystem::path regctl;
};

std::optional<ImageTools> FindImageTools(const std::string &flag,
                                         const char *env) {
  const std::string base = flag.empty() ? EnvOr(env, "") : flag;
  const std::string regctl = absl::GetFlag(FLAGS_regctl).empty()
                                 ? EnvOr("ARENA_REGCTL", "")
                                 : absl::GetFlag(FLAGS_regctl);
  if (base.empty() || regctl.empty()) {
    return std::nullopt;
  }
  return ImageTools{std::filesystem::absolute(base),
                    std::filesystem::absolute(regctl)};
}

// The manifest a layout holds. rules_oci writes index.json with one manifest
// and no tag on it, so the digest is the only name the image has.
std::optional<std::string> LayoutManifestDigest(
    const std::filesystem::path &layout) {
  const auto index = ReadFile(layout / "index.json");
  if (!index) {
    return std::nullopt;
  }
  static const std::regex kDigest(
      R"re("digest"\s*:\s*"(sha256:[0-9a-f]{64})")re");
  std::smatch match;
  if (!std::regex_search(*index, match, kDigest)) {
    return std::nullopt;
  }
  return match[1].str();
}

// Whether --push has somewhere to go. A reference names a registry when its
// first component looks like a host -- has a dot or a port, or is localhost --
// and one that does not is a local tag: `connect4-sandbox:1`, which is what
// the examples ship and all `play` needs. Pushing that would mean Docker Hub's
// library/, which is never what was meant, and is refused only after
// everything slow has already been done. So this is asked first.
bool CanPush(const std::string &image, std::string_view how_to_name_it) {
  if (!absl::GetFlag(FLAGS_push)) {
    return true;
  }
  const std::string first = image.substr(0, image.find('/'));
  const bool has_registry =
      image.find('/') != std::string::npos &&
      (first.find('.') != std::string::npos ||
       first.find(':') != std::string::npos || first == "localhost");
  if (has_registry) {
    return true;
  }
  LOG(ERROR) << "--push: \"" << image
             << "\" names no registry, so there is nowhere to push it. "
             << how_to_name_it;
  return false;
}

// GNU tar: --transform puts a tree where the image keeps it without staging
// a copy of it, and its S flag keeps that rewrite off the targets of the
// relative symlinks a vendored repository is full of.
bool HaveGnuTar() {
  const auto version =
      Capture("tar", {"--version"}, std::filesystem::current_path());
  if (!version || version->find("GNU tar") == std::string::npos) {
    LOG(ERROR) << "adding a layer to an image needs GNU tar on PATH";
    return false;
  }
  return true;
}

// Stacks |tars| on the built image, in order, applies |settings| (regctl's
// `image mod` flags: --env, --label), and delivers the result as |image|:
// pushed under --push, else loaded into the local daemon when there is one,
// else left in |archive|. |stage| is scratch for the layout being assembled.
bool DeliverImage(const ImageTools &tools, const std::filesystem::path &stage,
                  const std::vector<std::filesystem::path> &tars,
                  const std::vector<std::string> &settings,
                  const std::string &image,
                  const std::filesystem::path &archive) {
  const auto digest = LayoutManifestDigest(tools.base);
  if (!digest) {
    LOG(ERROR) << tools.base << " is not an OCI layout with a manifest in it";
    return false;
  }
  const std::string regctl = tools.regctl.string();
  const std::string layered =
      absl::StrCat("ocidir://", (stage / "layout").string(), ":image");
  std::string from =
      absl::StrCat("ocidir://", tools.base.string(), "@", *digest);
  // One `image mod` per layer -- --layer-add takes one -- with the settings
  // riding on the last. Compressing a large layer is most of the time taken.
  const std::size_t steps = std::max<std::size_t>(tars.size(), 1);
  for (std::size_t i = 0; i < steps; ++i) {
    std::vector<std::string> mod = {"image", "mod", from};
    if (from == layered) {
      mod.push_back("--replace");
    } else {
      mod.insert(mod.end(), {"--create", layered});
    }
    if (i < tars.size()) {
      mod.insert(mod.end(), {"--layer-add", "tar=" + tars[i].string()});
    }
    if (i + 1 == steps) {
      mod.insert(mod.end(), settings.begin(), settings.end());
    }
    if (RunInherit(regctl, mod, stage) != 0) {
      LOG(ERROR) << "regctl could not add to " << tools.base;
      return false;
    }
    from = layered;
  }

  if (absl::GetFlag(FLAGS_push)) {
    std::printf("Pushing %s...\n", image.c_str());
    std::fflush(stdout);
    if (RunInherit(regctl, {"image", "copy", layered, image}, stage) != 0) {
      LOG(ERROR) << "cannot push " << image
                 << "; regctl reads the registry logins docker keeps "
                    "(~/.docker/config.json)";
      return false;
    }
    return true;
  }
  // Not pushed: an archive `docker load` and `podman load` both read, and
  // loaded here when there is a daemon to load it into.
  if (RunInherit(
          regctl,
          {"image", "export", "--name", image, layered, archive.string()},
          stage) != 0) {
    LOG(ERROR) << "cannot write " << archive;
    return false;
  }
  const std::string docker = absl::GetFlag(FLAGS_docker);
  if (!process::ResolveExecutable(docker).empty() &&
      RunInherit(docker, {"load", "--input", archive.string()}, stage) == 0) {
    std::error_code ec;
    std::filesystem::remove(archive, ec);
    return true;
  }
  std::printf(
      "No docker daemon to load it into; the image is in %s:\n\n"
      "  docker load --input %s\n",
      archive.c_str(), archive.c_str());
  return true;
}

// Removes a scratch directory when the function that made it returns.
struct ScopedRemove {
  std::filesystem::path path;
  ~ScopedRemove() {
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);
  }
};

// ---------------------------------------------------------------------------
// up
// ---------------------------------------------------------------------------

int RunUp(const ArenaRunfiles &runfiles) {
  std::filesystem::path config_path;
  auto config = LoadConfig(&config_path);
  if (!config) {
    return 1;
  }
  std::string error;
  if (!CheckConfig(*config, config_path, &error)) {
    LOG(ERROR) << config_path.string() << ": " << error;
    return 1;
  }

  const std::filesystem::path server_bin =
      runfiles.Locate("game_arena/server/problem_server");
  if (server_bin.empty()) {
    return 1;
  }

  const std::filesystem::path data_dir =
      absl::GetFlag(FLAGS_data_dir).empty()
          ? StateDir(config->problem_id())
          : Resolve(absl::GetFlag(FLAGS_data_dir));
  std::error_code ec;
  std::filesystem::create_directories(data_dir, ec);
  if (ec) {
    LOG(ERROR) << "cannot create " << data_dir << ": " << ec.message();
    return 1;
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

  // Always gated: an empty registry is one nobody can write to, and
  // `kit --mint` adds a client to it. A coordinator with no registry
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

  auto server = process::Child::Start(server_bin.string(), server_args,
                                      process::ChildOptions{});
  if (!server) {
    LOG(ERROR) << "cannot start " << server_bin;
    return 1;
  }
  const std::filesystem::path pid_file = data_dir / "problem_server.pid";
  if (!WaitForPort(grpc_port, std::chrono::seconds(60))) {
    LOG(ERROR) << "problem_server did not open port " << grpc_port;
    server->Stop(std::chrono::seconds(5));
    return 1;
  }
  // Written only once the coordinator is actually serving: it is how `play`
  // knows the tournament in front of it is this one rather than whatever else
  // had the port.
  WriteFile(pid_file, absl::StrCat(server->pid(), "\n"));

  std::printf(
      "\n"
      "%s is up.\n"
      "  leaderboard   http://localhost:%d/\n"
      "  arena         localhost:%d   (writes need a token from %s)\n"
      "  state         %s\n"
      "\n"
      "It builds and runs nothing itself. A worker, on any host with docker "
      "and "
      "the\nsandbox image %s:\n"
      "  sandbox_worker --server=<this host>:%d\n"
      "\n"
      "A participant's kit (mints a token into the registry):\n"
      "  bazel run //:kit -- --mint=<client_id> --server=<this host>:%d\n"
      "\n"
      "Ctrl-C stops it.\n\n",
      config->display_name().empty() ? config->problem_id().c_str()
                                     : config->display_name().c_str(),
      http_port, grpc_port, clients.c_str(), data_dir.c_str(),
      config->sandbox().image().c_str(), grpc_port, grpc_port);
  std::fflush(stdout);

  int status = 0;
  while (!g_stop_requested) {
    if (const auto code = server->Poll()) {
      LOG(ERROR) << "problem_server exited with " << *code;
      status = 1;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
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
std::string ArenaOverrideBlock() {
  return "\n# Written by arena_tournament kit: the arena, trimmed to the "
         "packages\n# this workspace builds against, vendored in ./arena.\n"
         "local_path_override(\n"
         "    module_name = \"game_arena\",\n"
         "    path = \"arena\",\n"
         ")\n";
}

std::string WithoutArenaOverride(std::string text) {
  static const std::regex kArenaOverride(
      R"re((?:local_path|git|archive|single_version)_override\(\s*module_name\s*=\s*"game_arena"[^)]*\)\n?)re");
  return std::regex_replace(text, kArenaOverride, "");
}

std::string RerootedModuleFile(std::string text,
                               const std::filesystem::path &root) {
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

std::string KitBuildFile(const std::string &registry) {
  std::string text =
      "# Generated by arena_tournament kit. The arena's tools, reachable from\n"
      "# this workspace through @game_arena";
  if (!registry.empty()) {
    text += ", and the referee `arena_cli spar` plays with";
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
        "# What `arena_cli spar` referees your bot against a rival's or a\n"
        "# builtin with: the one the tournament's workers run.\n"
        "cc_binary(\n"
        "    name = \"match_referee\",\n"
        "    deps = [\n"
        "        \"",
        registry,
        "\",\n"
        "        \"@game_arena//game_arena/referee:referee_main\",\n"
        "    ],\n"
        ")\n");
  }
  return text;
}

std::string KitReadme(const proto::ProblemConfig &config,
                      const std::string &server, const std::string &http,
                      const std::string &client_id, bool has_token,
                      const std::vector<std::string> &files) {
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

  const std::string own = config.submission().files_submit_dir() + "/<you>";
  md << "\nYours is `" << own << "/`, made from `" << config.kit().starter_dir()
     << "/` the first time you run `arena_cli`. Every participant's "
        "implementation is a directory like it, named after them.\n";

  md << "\n## Iterating locally\n\n```sh\n";
  if (config.has_match()) {
    md << "arena_cli spar <name>           # pull <name>'s directory beside "
          "yours, build both, play them here\n"
          "arena_cli spar builtin:<name>   # or yours against a builtin\n";
    if (!config.match().placement_opponents().empty()) {
      md << "# builtins the arena rates you against first: "
         << absl::StrJoin(config.match().placement_opponents(), ", ") << "\n";
    }
  } else {
    md << "bazel build //...\n";
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
  md << "arena_cli submit --wait                # sends " << own
     << "/; again replaces it, once it builds\n"
        "arena_cli leaderboard\n";
  switch (config.source().visibility()) {
    case proto::SourcePolicy::OWN:
      md << "arena_cli source <your candidate_id>   # your own submissions "
            "only\n";
      break;
    case proto::SourcePolicy::NONE:
      break;
    default:
      md << "arena_cli source <name>                # pulled into "
         << config.submission().files_submit_dir() << "/<name>/\n";
      break;
  }
  md << "```\n\n";
  md << "`arena.textproto` is what those commands do when you do not say: the "
        "address, and where participants' directories live. It "
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
        "`arena_leaderboard`, `arena_source`); "
        "`arena_submit` takes file paths relative to this directory.\n";
  return md.str();
}

std::string JsonEscape(std::string_view s) {
  std::string out;
  for (const char c : s) {
    if (c == '"' || c == '\\') {
      out.push_back('\\');
    }
    out.push_back(c);
  }
  return out;
}

// mcp.json for a kit at |kit|: the path an agent's MCP client starts the
// server in, which is this host's for a directory and /kit inside an image.
std::string KitMcpJson(const std::filesystem::path &kit,
                       const std::string &server, const std::string &token,
                       const std::string &client_id) {
  std::string mcp = absl::StrCat(
      "{\n"
      "  \"mcpServers\": {\n"
      "    \"arena\": {\n"
      "      \"command\": \"bazel\",\n"
      "      \"args\": [\"run\", \"//:mcp_server\"],\n"
      "      \"cwd\": \"",
      JsonEscape(kit.string()),
      "\",\n"
      "      \"env\": {\n"
      "        \"ARENA_MCP_TARGET\": \"",
      JsonEscape(server), "\"");
  if (!token.empty()) {
    absl::StrAppend(&mcp, ",\n        \"ARENA_MCP_TOKEN\": \"",
                    JsonEscape(token), "\"");
  }
  if (!client_id.empty()) {
    absl::StrAppend(&mcp, ",\n        \"ARENA_NAME\": \"",
                    JsonEscape(client_id), "\"");
  }
  absl::StrAppend(&mcp, ",\n        \"ARENA_KIT\": \"",
                  JsonEscape(kit.string()), "\"");
  absl::StrAppend(&mcp, "\n      }\n    }\n  }\n}\n");
  return mcp;
}

// The build settings a kit at |kit| keeps in its .bazelrc.local: absolute
// paths, because bazel does not expand %workspace% inside a flag's value.
//
// |portable| is what makes a cache primed on this host hit inside an image of
// the kit. An action's key covers its environment, so PATH has to be bazel's
// fixed one rather than whoever-ran-it's, and a kit that is primed for an
// image carries the flag on the host too: the two builds have to be the same
// build. |vendored| points bazel at the dependencies `bazel vendor` copied
// into the kit, so that builds need no network and resolve exactly what the
// sandbox will.
std::string KitBuildSettings(const std::filesystem::path &kit, bool cached,
                             bool portable, bool vendored) {
  std::string rc;
  if (cached) {
    // Bounded: this lives inside someone's working directory, and a cache
    // that only grows is a surprise they find out about from `df`.
    absl::StrAppend(&rc,
                    "build --disk_cache=", (kit / ".arena" / "cache").string(),
                    "\ncommon --experimental_disk_cache_gc_max_size=",
                    absl::GetFlag(FLAGS_cache_limit), "\n");
  }
  if (portable) {
    absl::StrAppend(&rc, "build --incompatible_strict_action_env\n");
  }
  if (vendored) {
    absl::StrAppend(&rc, "common --vendor_dir=",
                    (kit / ".arena" / "vendor").string(), "\n");
  }
  return rc;
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
constexpr std::array<std::string_view, 3> kKitSurfaceFiles = {
    "MODULE.bazel", ".bazelversion", "protobuf_bzlmod_fixes.patch"};

// A runfiles tree holds a package's sources (symlinks into the checkout) and,
// beside them, anything built from it that something depends on -- arena_cli's
// own binary, for one, which is a symlink into bazel-out. A kit vendors the
// sources; the binary it gets is the builtin, installed once.
bool IsBuildOutput(const std::filesystem::path &path) {
  std::error_code ec;
  const std::filesystem::path real =
      std::filesystem::weakly_canonical(path, ec);
  return !ec && real.string().find("/bazel-out/") != std::string::npos;
}

// Copies that surface into <kit>/arena, from this tool's runfiles.
bool InstallArenaSurface(const ArenaRunfiles &runfiles,
                         const std::filesystem::path &out) {
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
  // The module's root package: the patch MODULE.bazel names is a label,
  // so the package has to exist. The arena's own root BUILD is not copied --
  // it exports files this surface does not have.
  return WriteFile(
      arena / "BUILD",
      "# The vendored arena's root package: the patch MODULE.bazel names.\n"
      "exports_files([\n"
      "    \"protobuf_bzlmod_fixes.patch\",\n"
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
// A layered kit image carries this same binary: the toolchain is hermetic and
// links libc++ statically, so it runs on any glibc the base is likely to have.
bool InstallBuiltins(const ArenaRunfiles &runfiles,
                     const std::filesystem::path &out) {
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
std::string KitConfigText(const proto::ProblemConfig &config,
                          const std::string &server,
                          const std::string &client_id) {
  proto::KitConfig kit;
  kit.set_server(server);
  kit.set_client_id(client_id);
  kit.set_submit_dir(config.submission().files_submit_dir());
  kit.set_starter_dir(config.kit().starter_dir());
  if (config.has_match()) {
    const proto::MatchSpec &match = config.match();
    kit.set_bot_binary(config.submission().harness().binary_name().empty()
                           ? "bot"
                           : config.submission().harness().binary_name());
    kit.set_game(match.game());
    // What sandbox/worker/order_job.cc gives the fleet's referee, so a game
    // played in a kit is bounded like a rated one.
    if (match.turn_timeout_ms() > 0) {
      kit.add_referee_flags(
          absl::StrCat("--turn_timeout_ms=", match.turn_timeout_ms()));
    }
    if (match.game_time_budget_ms() > 0) {
      kit.add_referee_flags(
          absl::StrCat("--game_time_budget_ms=", match.game_time_budget_ms()));
    }
    if (match.max_moves_per_game() > 0) {
      kit.add_referee_flags(
          absl::StrCat("--max_moves_per_game=", match.max_moves_per_game()));
    }
    if (!match.registry_options().empty()) {
      kit.add_referee_flags(
          absl::StrCat("--registry_options=",
                       kv_options::Format({match.registry_options().begin(),
                                           match.registry_options().end()})));
    }
  }
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

// ---------------------------------------------------------------------------
// kit --image
// ---------------------------------------------------------------------------

// The image of a kit is a build output: `bazel build //:kit_image` stacks the
// kit's tree on the arena's base, and that is the whole image -- anyone's,
// with no token in it and nothing built. What is added to it afterwards is
// not a build's business (see "images" above):
//
//   priming   the kit's dependencies vendored and its cache filled. That is
//             bazel run on a kit, which an action cannot do.
//   a token   a secret, which must never be an action's input.
//
// Each is a layer on the built image and either can be left out.

// The files of a kit that say whose it is and how it builds: what differs
// between the image bazel built and the one handed to a participant.
constexpr std::array<std::string_view, 5> kKitParticipantFiles = {
    "arena.env", "arena.textproto", "ARENA.md", "mcp.json", ".bazelrc.local"};

// Derives |image| from the built kit image: |kit|'s vendored dependencies and
// primed cache as one layer when there are any, then the participant's files
// as another, and the address and token in the environment -- so the tools
// work without sourcing arena.env, and `docker run -e ARENA_SERVER=...` moves
// the kit to another coordinator.
//
// |kit| is the same kit the image holds, written again on this host so there
// is something to run bazel on: same tool, same files, so the cache it fills
// is the cache the image's build will ask for.
int LayerKitImage(const ImageTools &tools, std::filesystem::path kit,
                  const std::string &image, const std::string &server,
                  const std::string &http, const std::string &token,
                  const std::string &client_id, bool cached, bool vendored) {
  if (kit.filename().empty()) {
    kit = kit.parent_path();
  }
  if (!HaveGnuTar()) {
    return 1;
  }
  // Beside the kit rather than inside it: it is as large as the cache again,
  // and it holds the token, so it does not outlive this function.
  const std::filesystem::path stage =
      kit.parent_path() / (kit.filename().string() + ".image");
  std::error_code ec;
  std::filesystem::remove_all(stage, ec);
  const std::filesystem::path participant = stage / "participant" / "kit";
  std::filesystem::create_directories(participant, ec);
  if (ec) {
    LOG(ERROR) << "cannot create " << stage << ": " << ec.message();
    return 1;
  }
  const ScopedRemove cleanup{stage};

  // uid 1000 is "ubuntu" in the arena's base, and the user the image runs
  // as: the vendored tree and the cache are theirs to write to.
  const std::vector<std::string> owner = {"--owner=1000", "--group=1000",
                                          "--numeric-owner"};
  std::printf("\nAdding to %s, as %s...\n", tools.base.filename().c_str(),
              image.c_str());
  std::fflush(stdout);
  std::vector<std::filesystem::path> tars;

  // What priming left in the kit. vendor/bazel-external is a symlink into
  // this host's output base; bazel makes its own in the image.
  std::vector<std::string> primed;
  if (vendored) {
    primed.push_back(".arena/vendor");
  }
  if (cached) {
    primed.push_back(".arena/cache");
  }
  if (!primed.empty()) {
    std::vector<std::string> primed_tar = {"--create", "--file",
                                           (stage / "primed.tar").string()};
    primed_tar.insert(primed_tar.end(), owner.begin(), owner.end());
    primed_tar.insert(primed_tar.end(),
                      {"--anchored", "--exclude=.arena/vendor/bazel-external",
                       "--transform=s,^,kit/,S", "--directory", kit.string()});
    primed_tar.insert(primed_tar.end(), primed.begin(), primed.end());
    if (RunInherit("tar", primed_tar, kit) != 0) {
      LOG(ERROR) << "cannot archive the primed cache in " << kit;
      return 1;
    }
    tars.push_back(stage / "primed.tar");
  }

  // The same files as the kit's, except the two that name where it is.
  std::vector<std::string> participant_tar = {
      "--create", "--file", (stage / "participant.tar").string()};
  participant_tar.insert(participant_tar.end(), owner.begin(), owner.end());
  participant_tar.insert(participant_tar.end(),
                         {"--directory", participant.parent_path().string()});
  const std::filesystem::path in_image = "/kit";
  for (const std::string_view name : kKitParticipantFiles) {
    const std::string file(name);
    if (file == "mcp.json") {
      WriteFile(participant / file,
                KitMcpJson(in_image, server, token, client_id));
    } else if (file == ".bazelrc.local") {
      WriteFile(participant / file,
                absl::StrCat("# Written by arena_tournament kit, for the kit "
                             "at /kit in this image.\n",
                             KitBuildSettings(in_image, cached,
                                              /*portable=*/true, vendored)));
    } else {
      std::filesystem::copy_file(kit / file, participant / file, ec);
      if (ec) {
        LOG(ERROR) << "cannot copy " << file << ": " << ec.message();
        return 1;
      }
    }
    participant_tar.push_back("kit/" + file);
  }
  if (RunInherit("tar", participant_tar, stage) != 0) {
    LOG(ERROR) << "cannot archive the participant's files";
    return 1;
  }
  tars.push_back(stage / "participant.tar");

  std::vector<std::string> settings = {
      "--env",
      "ARENA_SERVER=" + server,
      "--label",
      "org.opencontainers.image.title=arena kit for " +
          (client_id.empty() ? "a participant" : client_id),
      "--label",
      absl::StrCat("org.opencontainers.image.description=Development "
                   "environment for one participant of an arena tournament at ",
                   server, " (leaderboard: http://", http, "/)")};
  if (!token.empty()) {
    settings.insert(settings.end(), {"--env", "ARENA_TOKEN=" + token});
  }
  if (!client_id.empty()) {
    settings.insert(settings.end(), {"--env", "ARENA_NAME=" + client_id});
  }
  return DeliverImage(
             tools, stage, tars, settings, image,
             kit.parent_path() / (kit.filename().string() + ".image.tar"))
             ? 0
             : 1;
}

// What to do with a kit image, once there is one.
void PrintKitImageUsage(const std::string &image, bool has_token) {
  std::printf("\nMade %s%s. %s\n\n", image.c_str(),
              absl::GetFlag(FLAGS_push) ? " and pushed it" : "",
              has_token
                  ? "It is one participant's environment, token included; run "
                    "it wherever they work:"
                  : "There is no token in it, so it is anyone's: reads work, "
                    "and submitting takes `docker run -e ARENA_TOKEN=...`:");
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
}

int RunKit(const ArenaRunfiles &runfiles) {
  std::filesystem::path config_path;
  const auto config = LoadConfig(&config_path);
  if (!config) {
    return 1;
  }
  const std::filesystem::path root = WorkspaceRoot();

  // Whether --image can be honoured, settled before a token is minted for a
  // kit that then cannot be delivered. It is derived from the kit image bazel
  // built, which the kit_image_issue target carries.
  const std::string image = absl::GetFlag(FLAGS_image);
  const std::optional<ImageTools> image_tools =
      FindImageTools(absl::GetFlag(FLAGS_kit_base), "ARENA_KIT_BASE");
  const bool layered = image_tools.has_value();
  if (layered && image.empty()) {
    LOG(ERROR) << "kit_image_issue derives an image from the built one: pass "
                  "--image=TAG (and --push). `bazel build //:kit_image` is "
                  "the image itself, and //:kit the directory alone";
    return 1;
  }
  if (!image.empty() && !layered) {
    LOG(ERROR) << "--image adds to the kit image bazel built, which the "
                  "kit_image_issue target carries: bazel run "
                  "//:kit_image_issue -- --image="
               << image
               << " ... (or `bazel build //:kit_image` for the image with "
                  "nothing added)";
    return 1;
  }
  if (!image.empty() && !CanPush(image, "Pass --image=REGISTRY/NAME:TAG.")) {
    return 1;
  }
  if (layered && !absl::GetFlag(FLAGS_arena_override).empty()) {
    LOG(ERROR) << "--arena_override points a kit at a checkout on this host; "
                  "an image builds against the arena it carries";
    return 1;
  }
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
  // An image's kit is staged, not handed to anyone, so it is the same
  // directory every time: what the last image vendored and primed is in
  // .arena, and the next one starts from it. Everything else there is
  // rewritten, so a file the kit no longer has cannot break the priming build.
  const bool staged = layered && absl::GetFlag(FLAGS_out).empty();
  const std::filesystem::path out =
      staged ? StateDir(config->problem_id()) / "images" / "kit"
      : absl::GetFlag(FLAGS_out).empty()
          ? StateDir(config->problem_id()) / "kits" /
                (client_id.empty() ? "participant" : client_id)
          : Resolve(absl::GetFlag(FLAGS_out));
  std::error_code ec;
  if (staged) {
    for (const auto &entry : std::filesystem::directory_iterator(out, ec)) {
      if (entry.path().filename() != ".arena") {
        std::filesystem::remove_all(entry.path(), ec);
      }
    }
  } else if (std::filesystem::exists(out) && !std::filesystem::is_empty(out) &&
             !absl::GetFlag(FLAGS_force)) {
    LOG(ERROR) << out
               << " exists and is not empty; pass --force to write "
                  "into it anyway";
    return 1;
  }
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

  // What a participant starts from has to be in what they were given, or the
  // kit's first documented command fails and the participant finds out.
  if (!config->kit().starter_dir().empty() &&
      !std::filesystem::is_directory(out / config->kit().starter_dir())) {
    LOG(ERROR) << "kit.starter_dir \"" << config->kit().starter_dir()
               << "\" is not in the kit. Add it to the macro's kit_files";
    return 1;
  }

  bool prime = absl::GetFlag(FLAGS_prime_cache);
  if (prime && process::ResolveExecutable("bazel").empty()) {
    LOG(WARNING) << "no bazel on PATH; writing the kit without priming its "
                    "build cache";
    prime = false;
  }
  // An image's dependencies are vendored into the kit unless the problem lets
  // a kit's builds fetch: a dependency that resolves in the kit but not in the
  // sandbox is a submission that fails after it was tested. Part of priming,
  // so --prime_cache=false adds the token and nothing else, which takes
  // seconds, and leaves the image fetching and building cold as it was built.
  const bool vendored =
      layered && prime && !config->kit().allow_network_builds();

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
    absl::StrAppend(&env, "export ARENA_NAME=", client_id, "\n");
  }
  WriteFile(out / "arena.env", env);
  WriteFile(out / "arena.textproto",
            KitConfigText(*config, server, client_id));
  if (!InstallBuiltins(runfiles, out)) {
    return 1;
  }

  // Where the kit will be used from: here, unless it is being written for
  // somewhere else -- the tree of a kit image is made in a build action's
  // scratch directory and lives at /kit.
  const std::filesystem::path home =
      absl::GetFlag(FLAGS_kit_path).empty()
          ? out
          : std::filesystem::path(absl::GetFlag(FLAGS_kit_path));
  WriteFile(out / "mcp.json", KitMcpJson(home, server, token, client_id));

  // bazel-* are symlinks the priming build leaves behind; .bazelrc.local
  // names this host's paths; .arena/cache is a bazel disk cache and
  // .arena/bin this host's binaries, neither of which a git history should
  // carry.
  // The vendored arena is a module, not part of this workspace: without this
  // `bazel build //...` here would try to load its packages as the kit's own
  // and fail on labels that only resolve inside it.
  WriteFile(out / ".bazelignore",
            "# The arena is a bazel module of its own (MODULE.bazel points at\n"
            "# it); //... is your workspace, not it.\narena\n");
  WriteFile(out / ".gitignore",
            "bazel-*\n.bazelrc.local\n.arena/cache/\n.arena/bin/\n");
  // This host's settings, in the file the problem's own .bazelrc is told to
  // try-import: an absolute path, because bazel does not expand %workspace%
  // inside a flag's value, and uncommitted, because it names this machine.
  const std::string arena_override = absl::GetFlag(FLAGS_arena_override);
  const std::filesystem::path cache = home / ".arena" / "cache";
  const std::filesystem::path vendor = home / ".arena" / "vendor";
  std::string local = absl::StrCat(
      home == out ? "# Written by arena_tournament kit. This host's paths; "
                    "not committed,\n# and not copied into an image of the "
                    "kit.\n"
                  : "# Written by arena_tournament kit. Yours: bazel reads it "
                    "after .bazelrc.\n",
      KitBuildSettings(home, prime, /*portable=*/layered, vendored));
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
          line.find("--vendor_dir=" + vendor.string()) !=
              std::string_view::npos ||
          line.find("--incompatible_strict_action_env") !=
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

  if (prime) {
    // For an image, the build that fills the cache has to be the build the
    // image will run, or nothing in it hits. This host's own rc files are
    // what would make it a different one -- a remote executor's platform
    // properties are part of every action's key, and a container has no
    // ~/.bazelrc -- so they are left out, and the kit's .bazelrc.local
    // carries the rest (KitBuildSettings).
    std::vector<std::string> startup =
        layered ? std::vector<std::string>{"--nohome_rc", "--nosystem_rc"}
                : std::vector<std::string>{};
    if (!absl::GetFlag(FLAGS_prime_bazelrc).empty()) {
      startup.push_back("--bazelrc=" +
                        Resolve(absl::GetFlag(FLAGS_prime_bazelrc)).string());
    }
    const auto bazel = [&](std::vector<std::string> command) {
      command.insert(command.begin(), startup.begin(), startup.end());
      return RunInherit("bazel", command, out);
    };
    if (vendored) {
      std::printf("\nVendoring the kit's dependencies into %s...\n",
                  vendor.c_str());
      std::fflush(stdout);
      if (bazel({"vendor", "//..."}) != 0) {
        LOG(ERROR) << "cannot vendor the kit's dependencies";
        return 1;
      }
    }
    std::printf(
        "\nBuilding the kit once, into %s (the first time takes a while; "
        "--prime_cache=false skips it)...\n",
        cache.c_str());
    std::fflush(stdout);
    const int code = bazel({"build", "//..."});
    if (layered) {
      // One server per kit otherwise, each holding this build in memory.
      bazel({"shutdown"});
    }
    if (code != 0) {
      LOG(ERROR) << "the kit does not build (exit " << code
                 << "); kit_files is probably missing a BUILD file or a "
                    "source one of them depends on";
      return 1;
    }
    std::printf("Kit builds, and its cache is warm.\n");
  }

  if (image.empty()) {
    return 0;
  }
  if (server.rfind("localhost", 0) == 0 || server.rfind("127.", 0) == 0) {
    LOG(WARNING) << "--server=" << server
                 << " is baked into the image, and inside a container that "
                    "is the container itself; pass --server=<host>:<port> "
                    "as the coordinator is reached from where the kit runs, "
                    "or override with `docker run -e ARENA_SERVER=...`";
  }
  if (const int code =
          LayerKitImage(*image_tools, out, image, server, http, token,
                        client_id, /*cached=*/prime, vendored);
      code != 0) {
    return code;
  }
  PrintKitImageUsage(image, !token.empty());
  return 0;
}

// ---------------------------------------------------------------------------
// play
// ---------------------------------------------------------------------------

// The value of KEY=... in a shell-style env file (`export KEY=value` lines).
std::string EnvFileValue(const std::filesystem::path &file,
                         std::string_view key) {
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

std::string TailOf(const std::filesystem::path &file, int lines) {
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
// they already have the flags, the checks and the messages. What is new here
// is the shell, and the one worker a tournament on its own does not have.
int RunPlay(const ArenaRunfiles &runfiles) {
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
  std::printf("Starting the tournament (log: %s).\n", log.c_str());
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

  // A worker, and the image it builds and runs every submission in. The image
  // holds the problem's tree, so it is made every time -- a cached build --
  // by running the target that carries its base, which this one must not.
  const std::filesystem::path worker_bin =
      runfiles.Locate("game_arena/sandbox/worker/sandbox_worker");
  if (worker_bin.empty() ||
      RunInherit("bazel", {"run", EnvOr("ARENA_SANDBOX_LOAD_TARGET", "")},
                 WorkspaceRoot()) != 0) {
    LOG(ERROR) << "cannot make the sandbox image";
    up->Stop(std::chrono::seconds(10));
    return 1;
  }
  process::ChildOptions worker_options;
  worker_options.stdout_path = data_dir / "logs" / "worker.log";
  worker_options.stderr_path = worker_options.stdout_path;
  worker_options.extra_env = {"ARENA_WORK_DIR=" + (data_dir / "work").string()};
  auto worker = process::Child::Start(
      worker_bin.string(), {absl::StrCat("--server=localhost:", grpc_port)},
      worker_options);
  if (!worker) {
    LOG(ERROR) << "cannot start " << worker_bin;
    up->Stop(std::chrono::seconds(10));
    return 1;
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
      "    arena_cli submit --wait\n"
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
      "ARENA_NAME=" + client_id,
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
  // The worker first: an order in flight is cancelled rather than orphaned.
  worker->Stop(std::chrono::seconds(10));
  const int status = up->Stop(std::chrono::seconds(20));
  return tournament_died ? 1 : (status == 0 ? 0 : 1);
}

void PrintUsage() {
  std::fprintf(stderr,
               "usage: arena_tournament <up|kit|check|play> "
               "--problem_config=<path> [flags]\n"
               "  up      run the coordinator\n"
               "  play    up in the background, a kit for you, a shell in it\n"
               "  kit     write a participant's workspace (--out, --mint, "
               "--image)\n"
               "  check   validate the config\n");
}

}  // namespace

int main(int argc, char **argv) {
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
  if (command == "play") {
    return RunPlay(runfiles);
  }
  PrintUsage();
  return 2;
}
