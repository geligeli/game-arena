// Tournament tooling; names no problem: every label comes from the caller.

#include <arpa/inet.h>
#include <google/protobuf/text_format.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <boost/json/array.hpp>
#include <boost/json/object.hpp>
#include <boost/json/parse.hpp>
#include <boost/json/serialize.hpp>
#include <boost/json/value.hpp>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <optional>
#include <regex>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
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
#include "game_arena/sandbox/common/docker.h"
#include "game_arena/server/client_registry.h"
#include "game_arena/server/problem_config.h"
#include "game_arena/tools/sandbox_priming.h"
#include "rules_cc/cc/runfiles/runfiles.h"

ABSL_FLAG(std::string, problem_config, "",
          "The problem's .textproto (required). "
          "Relative to the workspace root under `bazel run`, else to the "
          "current directory");

ABSL_FLAG(std::string, data_dir, "",
          "up: where the coordinator keeps submissions, ratings and the "
          "workers' checkouts. Default: $ARENA_STATE_DIR/<problem_id>, and "
          "$ARENA_STATE_DIR defaults to ~/.arena. Kept out of the repo so "
          "`bazel test //...` there never descends into a worker's clone");
ABSL_FLAG(std::string, swiss_from, "",
          "swiss: the tournament whose every version is re-ranked. Default: "
          "$ARENA_STATE_DIR/<problem_id>");
ABSL_FLAG(std::string, import_versions_from, "",
          "up, with submission.versions: first make every submission in this "
          "data dir's job log a version here (see problem_server)");
ABSL_FLAG(int, swiss_rounds, 0, "swiss: rounds. 0: ceil(log2(entries)) + 3");
ABSL_FLAG(int, swiss_games, 2, "swiss: games per match");
ABSL_FLAG(int, grpc_port, 50051, "up: the Arena and SandboxFleet port");
ABSL_FLAG(int, http_port, 8090, "up: the leaderboard port");
ABSL_FLAG(std::string, clients, "",
          "up/kit: the client registry. up passes it to the coordinator, "
          "creating it empty if it does not exist; kit --mint appends to it, "
          "and the coordinator reads it on the token's first use. Default: "
          "<data_dir>/clients.textproto");

ABSL_FLAG(std::string, out, "",
          "kit: directory to write the kit into (created). Default: "
          "<data_dir>/kits/<client_id>; for --image, <data_dir>/images/kit, "
          "which keeps what was vendored and primed between images");
ABSL_FLAG(std::string, server, "",
          "kit: the arena address baked into the kit. Empty: the kit takes "
          "$ARENA_SERVER when it runs");
ABSL_FLAG(std::string, http, "",
          "kit: the leaderboard address named in the kit's README");
ABSL_FLAG(std::string, mint, "",
          "kit: mint a token for this client_id, append the client to "
          "--clients, and bake the token into the kit. Not with --image");
ABSL_FLAG(std::string, token, "",
          "kit: bake this existing token in instead of minting one. Not "
          "with --image");
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
          "warm and a missing kit file is found here rather than by them. "
          "sandbox: add a disk cache of building it to the image, which fills "
          "a worker's empty cache volume; without it, only the vendored "
          "dependencies");
ABSL_FLAG(std::string, prime_bazelrc, "",
          "kit, sandbox: a bazelrc for the priming build and vendoring only, "
          "e.g. a remote cache that executes locally. Not copied into what "
          "is made");
ABSL_FLAG(bool, force, false, "kit: write into a non-empty --out");
ABSL_FLAG(std::string, kit_path, "",
          "kit: where the kit will be used from, when that is not --out: the "
          "path baked into mcp.json and .bazelrc.local. The kit inside an "
          "image is written somewhere else and lives at /kit");
ABSL_FLAG(std::string, image, "",
          "kit: derive an image with this tag from the kit image bazel built "
          "(--kit_base): the kit's dependencies vendored and its build cache "
          "primed. No docker involved; run it as //:kit_image_issue, the "
          "target that carries the built image. --push pushes it");
ABSL_FLAG(std::string, kit_base, "",
          "kit --image: the OCI layout that is added to -- the kit image "
          "`bazel build //:kit_image` makes. Supplied by the arena_problem "
          "macro's kit_image_issue target; default $ARENA_KIT_BASE");
ABSL_FLAG(std::string, regctl, "",
          "kit --image, sandbox: the regctl binary that adds the layers and "
          "pushes. Supplied with the base; default $ARENA_REGCTL");

ABSL_FLAG(std::string, shell, "",
          "play: the shell to drop into the kit. Default: $SHELL, else bash");

ABSL_FLAG(bool, push, false,
          "kit --image, sandbox: push the result to its registry, with the "
          "logins docker keeps. Without it the image is loaded into the local "
          "daemon, or left as an archive when there is none");
ABSL_FLAG(std::string, docker, "docker",
          "up: the docker binary the sandbox image is looked for with. "
          "image, --image: the one an unpushed image is loaded with");

namespace {

namespace proto = tournament_arena::proto;
using rules_cc::cc::runfiles::Runfiles;

volatile std::sig_atomic_t g_stop_requested = 0;

extern "C" void OnStopSignal(int /*signum*/) { g_stop_requested = 1; }

std::string EnvOr(const char* name, std::string fallback) {
  const char* value = std::getenv(name);
  return value != nullptr && *value != '\0' ? std::string(value) : fallback;
}

std::filesystem::path WorkspaceRoot() {
  const std::string root = EnvOr("BUILD_WORKSPACE_DIRECTORY", "");
  return root.empty() ? std::filesystem::current_path()
                      : std::filesystem::path(root);
}

auto Resolve(const std::string& path) -> std::filesystem::path {
  const std::filesystem::path p(path);
  return p.is_absolute() ? p : WorkspaceRoot() / p;
}

std::filesystem::path PathFlag(const absl::Flag<std::string>& flag,
                               const std::filesystem::path& fallback) {
  const std::string value = absl::GetFlag(flag);
  return value.empty() ? fallback : Resolve(value);
}

std::filesystem::path StateDir(std::string_view problem_id) {
  const std::filesystem::path base =
      EnvOr("ARENA_STATE_DIR", EnvOr("HOME", "/tmp") + "/.arena");
  return base / std::string(problem_id);
}

std::optional<std::string> ReadFile(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return std::nullopt;
  }
  return std::string((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());
}

bool WriteFile(const std::filesystem::path& path, std::string_view text) {
  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out << text;
  return static_cast<bool>(out);
}

// With this process's stdout and stderr; -1 when it could not be started.
int RunInherit(const std::string& executable,
               const std::vector<std::string>& arguments,
               const std::filesystem::path& cwd) {
  return process::RunCommand(executable, arguments, {.cwd = cwd}).exit_code;
}

// Its stdout, or nullopt when it cannot start or exits non-zero.
std::optional<std::string> Capture(const std::string& executable,
                                   const std::vector<std::string>& arguments,
                                   const std::filesystem::path& cwd) {
  const std::filesystem::path out =
      std::filesystem::temp_directory_path() /
      absl::StrCat("arena_tournament_", ::getpid(), "_",
                   std::chrono::steady_clock::now().time_since_epoch().count());
  const process::RunResult result = process::RunCommand(
      executable, arguments,
      {.cwd = cwd, .stdout_path = out, .stderr_path = "/dev/null"},
      std::chrono::seconds(60));
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
        ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0;
    ::close(fd);
    if (open) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }
  return false;
}

std::optional<proto::ProblemConfig> LoadConfig(
    std::filesystem::path* config_path) {
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

class ArenaRunfiles {
 public:
  explicit ArenaRunfiles(const char* argv0) {
    std::string error;
    runfiles_.reset(Runfiles::Create(argv0, BAZEL_CURRENT_REPOSITORY, &error));
    if (!runfiles_) {
      LOG(WARNING) << "runfiles unavailable: " << error;
    }
  }

  auto Locate(const std::string& path) const -> std::filesystem::path {
    const std::filesystem::path found = LocateSource(path);
    if (found.empty()) {
      LOG(ERROR) << "cannot find " << path
                 << " in runfiles; is this running under bazel run?";
    }
    return found;
  }

  // A $(rlocationpath) as the macro wrote it: any repository's file,
  // generated ones included.
  auto LocateRunfile(const std::string& rlocation) const
      -> std::filesystem::path {
    std::vector<std::string> candidates;
    if (runfiles_) {
      candidates.push_back(runfiles_->Rlocation(rlocation));
    }
    const std::string dir = EnvOr("RUNFILES_DIR", "");
    if (!dir.empty()) {
      candidates.push_back(dir + "/" + rlocation);
    }
    for (const std::string& candidate : candidates) {
      if (!candidate.empty() && std::filesystem::exists(candidate)) {
        return std::filesystem::absolute(candidate);
      }
    }
    return {};
  }

  // Locate without the error: a missing source is the caller's to report.
  auto LocateSource(const std::string& path) const -> std::filesystem::path {
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
    for (const std::string& candidate : candidates) {
      if (!candidate.empty() && std::filesystem::exists(candidate)) {
        return std::filesystem::absolute(candidate);
      }
    }
    return {};
  }

 private:
  std::unique_ptr<Runfiles> runfiles_;
};

bool CheckConfig(const proto::ProblemConfig& config,
                 const std::filesystem::path& config_path, std::string* error) {
  // Only under `bazel run` is there a tree: a test has the config alone.
  const std::string tree = EnvOr("BUILD_WORKSPACE_DIRECTORY", "").empty()
                               ? ""
                               : config_path.parent_path().string();
  const auto in_tree = [&tree](const std::string& path) {
    return std::filesystem::exists(std::filesystem::path(tree) / path);
  };
  const std::string& submit_dir = config.submission().files_submit_dir();
  if (!submit_dir.empty()) {
    bool names_submission = false;
    for (const std::string& target : config.build().targets()) {
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
  // Offline, a build with no lockfile re-resolves against the registry: fails.
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

struct ImageTools {
  std::filesystem::path base;
  std::filesystem::path regctl;
};

std::optional<ImageTools> FindImageTools(const std::string& flag,
                                         const char* env) {
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

// rules_oci writes one untagged manifest: its digest is the image's only name.
std::optional<std::string> LayoutManifestDigest(
    const std::filesystem::path& layout) {
  const auto index = ReadFile(layout / "index.json");
  if (!index) {
    return std::nullopt;
  }
  boost::system::error_code ec;
  const boost::json::value parsed = boost::json::parse(*index, ec);
  const boost::json::value* digest =
      ec ? nullptr : parsed.find_pointer("/manifests/0/digest", ec);
  if (digest == nullptr || !digest->is_string()) {
    return std::nullopt;
  }
  return std::string(digest->get_string());
}

// A bare `name:tag` would push to Docker Hub: refused before the slow part.
bool CanPush(const std::string& image, std::string_view how_to_name_it) {
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

// GNU tar: --transform's S flag keeps the rewrite off symlink targets.
bool HaveGnuTar() {
  const auto version =
      Capture("tar", {"--version"}, std::filesystem::current_path());
  if (!version || version->find("GNU tar") == std::string::npos) {
    LOG(ERROR) << "adding a layer to an image needs GNU tar on PATH";
    return false;
  }
  return true;
}

// |tars| on the built image: pushed, else loaded, else left in |archive|.
bool DeliverImage(const ImageTools& tools, const std::filesystem::path& stage,
                  const std::vector<std::filesystem::path>& tars,
                  const std::string& image,
                  const std::filesystem::path& archive) {
  const auto digest = LayoutManifestDigest(tools.base);
  if (!digest) {
    LOG(ERROR) << tools.base << " is not an OCI layout with a manifest in it";
    return false;
  }
  const std::string regctl = tools.regctl.string();
  const std::string layered =
      absl::StrCat("ocidir://", (stage / "layout").string(), ":image");
  // One `image mod` per layer: --layer-add takes one.
  std::string from =
      absl::StrCat("ocidir://", tools.base.string(), "@", *digest);
  for (const std::filesystem::path& tar : tars) {
    std::vector<std::string> mod = {"image", "mod", from};
    if (from == layered) {
      mod.push_back("--replace");
    } else {
      mod.insert(mod.end(), {"--create", layered});
    }
    mod.insert(mod.end(), {"--layer-add", "tar=" + tar.string()});
    if (RunInherit(regctl, mod, stage) != 0) {
      LOG(ERROR) << "regctl could not add " << tar << " to " << tools.base;
      return false;
    }
    from = layered;
  }

  if (absl::GetFlag(FLAGS_push)) {
    std::printf("Pushing %s...\n", image.c_str());
    if (RunInherit(regctl, {"image", "copy", layered, image}, stage) != 0) {
      LOG(ERROR) << "cannot push " << image
                 << "; regctl reads the registry logins docker keeps "
                    "(~/.docker/config.json)";
      return false;
    }
    return true;
  }
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

struct ScopedRemove {
  std::filesystem::path path;
  ~ScopedRemove() {
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);
  }
};

std::vector<std::string> PrimeStartup(std::vector<std::string> startup) {
  if (!absl::GetFlag(FLAGS_prime_bazelrc).empty()) {
    startup.push_back("--bazelrc=" +
                      Resolve(absl::GetFlag(FLAGS_prime_bazelrc)).string());
  }
  return startup;
}

int Bazel(const std::vector<std::string>& startup,
          std::vector<std::string> command, const std::filesystem::path& cwd) {
  command.insert(command.begin(), startup.begin(), startup.end());
  return RunInherit("bazel", command, cwd);
}

// |swiss|: a re-rank of the tournament in the default state dir, in a data
// dir of its own, rather than the tournament itself.
int RunUp(const ArenaRunfiles& runfiles, bool swiss = false) {
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

  const std::filesystem::path state = StateDir(config->problem_id());
  const std::filesystem::path data_dir = PathFlag(
      FLAGS_data_dir,
      swiss ? absl::StrCat(
                  state.string(), "-swiss-",
                  std::chrono::duration_cast<std::chrono::seconds>(
                      std::chrono::system_clock::now().time_since_epoch())
                      .count())
            : state.string());
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

  // Always gated: with no registry the coordinator takes anyone's token.
  const std::filesystem::path clients =
      PathFlag(FLAGS_clients, data_dir / "clients.textproto");
  if (!std::filesystem::exists(clients) &&
      !WriteFile(clients,
                 "# Client registry: one `client { ... }` per participant.\n"
                 "# `arena_tournament kit --mint=<id>` appends here.\n")) {
    LOG(ERROR) << "cannot create " << clients;
    return 1;
  }

  const int grpc_port = absl::GetFlag(FLAGS_grpc_port);
  const int http_port = absl::GetFlag(FLAGS_http_port);
  std::vector<std::string> server_args = {
      "--problem_config=" + effective.string(),
      "--data_dir=" + data_dir.string(),
      absl::StrCat("--grpc_port=", grpc_port),
      absl::StrCat("--http_port=", http_port),
      "--clients=" + clients.string(),
  };
  if (const std::string from = absl::GetFlag(FLAGS_import_versions_from);
      !from.empty()) {
    server_args.push_back("--import_versions_from=" + Resolve(from).string());
  }
  if (swiss) {
    server_args.push_back("--swiss_from=" +
                          PathFlag(FLAGS_swiss_from, state).string());
    server_args.push_back(
        absl::StrCat("--swiss_rounds=", absl::GetFlag(FLAGS_swiss_rounds)));
    server_args.push_back(
        absl::StrCat("--swiss_games=", absl::GetFlag(FLAGS_swiss_games)));
  }
  // arena_problem's replay_assets, by runfiles path, and the module among
  // them; the coordinator serves the files and checks the module is one.
  std::vector<std::string> assets;
  for (const std::string_view rlocation : absl::StrSplit(
           EnvOr("ARENA_REPLAY_ASSETS", ""), ' ', absl::SkipEmpty())) {
    const std::filesystem::path found =
        runfiles.LocateRunfile(std::string(rlocation));
    if (found.empty()) {
      LOG(ERROR) << "cannot find replay asset " << rlocation << " in runfiles";
      return 1;
    }
    assets.push_back(found.string());
  }
  if (!assets.empty()) {
    server_args.push_back("--replay_assets=" + absl::StrJoin(assets, ","));
  }
  if (const std::string module = EnvOr("ARENA_REPLAY_MODULE", "");
      !module.empty()) {
    server_args.push_back("--replay_module=" + module);
  }

  struct sigaction action{};
  action.sa_handler = OnStopSignal;
  ::sigaction(SIGINT, &action, nullptr);
  ::sigaction(SIGTERM, &action, nullptr);

  auto server = process::Child::Start(server_bin.string(), server_args, {});
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
  // Only once serving: it is how `play` knows the port is this tournament's.
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

// A kit builds against the arena vendored in it, whatever the problem pins.
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
                               const std::filesystem::path& root) {
  static const std::regex kOverride(
      R"re(local_path_override\(([^)]*?)path\s*=\s*"([^"]+)")re");
  std::string out;
  auto begin = std::sregex_iterator(text.begin(), text.end(), kOverride);
  std::size_t last = 0;
  for (auto it = begin; it != std::sregex_iterator(); ++it) {
    const std::smatch& m = *it;
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

std::string KitBuildFile(const std::string& registry) {
  std::string text = "# Generated by arena_tournament kit";
  if (!registry.empty()) {
    text += ": the referee `arena_cli spar` plays with";
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
      "# build, and `arena_cli mcp` is the same operations for an agent.\n";
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

std::string KitReadme(const proto::ProblemConfig& config,
                      const std::string& server, const std::string& http,
                      const std::string& client_id, bool has_token,
                      const std::vector<std::string>& files) {
  std::ostringstream md;
  const std::string title = config.display_name().empty()
                                ? config.problem_id()
                                : config.display_name();
  md << "# " << title << "\n\n" << config.description() << "\n";

  md << "## Your solution\n\n";
  const auto& submission = config.submission();
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
  for (const std::string& file : files) {
    md << "- `" << file << "`\n";
  }

  const std::string own = config.submission().files_submit_dir() + "/<you>";
  md << "\nYours is `" << own << "/`, made from `" << config.kit().starter_dir()
     << "/` the first time you run `arena_cli` -- or, with `ARENA_RESTORE=1`, "
        "from your last submission. Every participant's "
        "implementation is a directory like it, named after them.\n";

  md << "\n## Iterating locally\n\n```sh\n";
  if (config.has_match()) {
    if (config.match().players() > 2) {
      md << "arena_cli spar <name> <name2>   # a game seats "
         << config.match().players()
         << ": a rival for each other seat, pulled and built beside yours\n"
            "arena_cli spar builtin:<name>   # a builtin fills every seat "
            "left\n";
    } else {
      md << "arena_cli spar <name>           # pull <name>'s directory beside "
            "yours, build both, play them here\n"
            "arena_cli spar builtin:<name>   # or yours against a builtin\n";
    }
    if (!config.match().placement_opponents().empty()) {
      md << "# builtins the arena rates you against first: "
         << absl::StrJoin(config.match().placement_opponents(), ", ") << "\n";
    }
  } else {
    md << "bazel build //...\n";
  }
  md << "```\n";
  if (!config.build().bazel_flags().empty()) {
    md << "\nRated builds use `bazel build "
       << absl::StrJoin(config.build().bazel_flags(), " ") << "`"
       << (config.has_match() ? ", and so does `arena_cli spar`" : "")
       << ". A plain `bazel build` does not: build with these flags to "
          "measure your code as it will run.\n";
  }

  md << "\n## Submitting\n\n"
        "The arena is at `"
     << (server.empty() ? "$ARENA_SERVER" : server) << "`";
  if (!http.empty()) {
    md << "; the leaderboard at <http://" << http << "/>";
  }
  md << ".";
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
  md << "\nFor an agent, `arena_cli mcp` serves the same operations as MCP "
        "tools on stdio (`arena_rules`, `arena_submit`, `arena_job`, "
        "`arena_leaderboard`, `arena_source`, `arena_spar`), and `.mcp.json` "
        "registers it; `arena_submit` takes file paths relative to this "
        "directory.\n";
  return md.str();
}

// |kit| is where the kit is used from: this host's path, or /kit in an image.
std::string KitMcpJson(const std::filesystem::path& kit,
                       const std::string& server, const std::string& token,
                       const std::string& client_id) {
  boost::json::object env{{"ARENA_KIT", kit.string()}};
  for (const auto& [key, value] :
       {std::pair{"ARENA_SERVER", server}, std::pair{"ARENA_TOKEN", token},
        std::pair{"ARENA_NAME", client_id}}) {
    if (!value.empty()) {
      env[key] = value;
    }
  }
  const boost::json::object arena{
      {"command", (kit / ".arena" / "bin" / "arena_cli").string()},
      {"args", boost::json::array{"mcp"}},
      {"env", std::move(env)},
  };
  return boost::json::serialize(
             boost::json::object{{"mcpServers", {{"arena", arena}}}}) +
         "\n";
}

// Absolute paths: bazel does not expand %workspace% inside a flag's value.
// Strict action env: PATH is in every action's key; a primed cache must hit.
std::string KitBuildSettings(const std::filesystem::path& kit, bool cached,
                             bool vendored) {
  std::string rc;
  if (cached) {
    // Bounded: it lives in someone's working directory.
    absl::StrAppend(&rc,
                    "build --disk_cache=", (kit / ".arena" / "cache").string(),
                    "\ncommon --experimental_disk_cache_gc_max_size=",
                    absl::GetFlag(FLAGS_cache_limit), "\n");
  }
  absl::StrAppend(&rc, "build --incompatible_strict_action_env\n");
  if (vendored) {
    absl::StrAppend(&rc, "common --vendor_dir=",
                    (kit / ".arena" / "vendor").string(), "\n");
  }
  return rc;
}

// //:kit_surface's packages; //game_arena:kit_surface_test keeps them closed.
constexpr std::array<std::string_view, 6> kKitSurfaceDirs = {
    "game_arena/proto", "game_arena/referee",   "game_arena/client",
    "game_arena/cli",   "game_arena/standings", "game_arena/common/kv_options",
};

// The module's root files, for a local_path_override to it.
constexpr std::array<std::string_view, 3> kKitSurfaceFiles = {
    "MODULE.bazel", ".bazelversion", "protobuf_bzlmod_fixes.patch"};

// Runfiles hold built files too (arena_cli, in bazel-out); a kit gets sources.
bool IsBuildOutput(const std::filesystem::path& path) {
  std::error_code ec;
  const std::filesystem::path real =
      std::filesystem::weakly_canonical(path, ec);
  return !ec && real.string().find("/bazel-out/") != std::string::npos;
}

bool InstallArenaSurface(const ArenaRunfiles& runfiles,
                         const std::filesystem::path& out) {
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
    for (const auto& entry :
         std::filesystem::recursive_directory_iterator(from, ec)) {
      if (!entry.is_regular_file() || IsBuildOutput(entry.path())) {
        continue;
      }
      // Lexical: relative() resolves symlinks; runfiles link to the checkout.
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
  // MODULE.bazel names its patch by label, so the root package must exist.
  return WriteFile(
      arena / "BUILD",
      "# The vendored arena's root package: the patch MODULE.bazel names.\n"
      "exports_files([\n"
      "    \"protobuf_bzlmod_fixes.patch\",\n"
      "])\n");
}

bool InstallBuiltins(const ArenaRunfiles& runfiles,
                     const std::filesystem::path& out) {
  const std::filesystem::path cli = runfiles.Locate("game_arena/cli/arena_cli");
  if (cli.empty()) {
    LOG(ERROR) << "cannot find arena_cli to install into the kit";
    return false;
  }
  const std::filesystem::path bin = out / ".arena" / "bin";
  std::error_code ec;
  std::filesystem::create_directories(bin, ec);
  const std::filesystem::path target = bin / "arena_cli";
  // `play` rewrites the kit every run: skip a large binary that is there.
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

std::string KitConfigText(const proto::ProblemConfig& config,
                          const std::string& server,
                          const std::string& client_id) {
  proto::KitConfig kit;
  kit.set_server(server);
  kit.set_client_id(client_id);
  kit.set_submit_dir(config.submission().files_submit_dir());
  kit.set_starter_dir(config.kit().starter_dir());
  *kit.mutable_bazel_flags() = config.build().bazel_flags();
  if (config.has_match()) {
    const proto::MatchSpec& match = config.match();
    kit.set_bot_binary(config.submission().harness().binary_name().empty()
                           ? "bot"
                           : config.submission().harness().binary_name());
    kit.set_game(match.game());
    kit.set_players(static_cast<int>(match.players()));
    // Bounded as sandbox/worker/order_job.cc bounds a rated game.
    for (const auto& [flag, value] :
         {std::pair{"--turn_timeout_ms=", match.turn_timeout_ms()},
          std::pair{"--game_time_budget_ms=", match.game_time_budget_ms()},
          std::pair{"--max_moves_per_game=", match.max_moves_per_game()}}) {
      if (value > 0) {
        kit.add_referee_flags(absl::StrCat(flag, value));
      }
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

// |kit| is the image's kit, written again here so its primed cache hits there.
int LayerKitImage(const ImageTools& tools, std::filesystem::path kit,
                  const std::string& image, bool cached, bool vendored) {
  if (kit.filename().empty()) {
    kit = kit.parent_path();
  }
  if (!HaveGnuTar()) {
    return 1;
  }
  const std::filesystem::path stage =
      kit.parent_path() / (kit.filename().string() + ".image");
  std::error_code ec;
  std::filesystem::remove_all(stage, ec);
  std::filesystem::create_directories(stage, ec);
  if (ec) {
    LOG(ERROR) << "cannot create " << stage << ": " << ec.message();
    return 1;
  }
  const ScopedRemove cleanup{stage};
  WriteFile(stage / ".bazelrc.local",
            absl::StrCat("# Written by arena_tournament kit, for the kit at "
                         "/kit in this image.\n",
                         KitBuildSettings("/kit", cached, vendored)));

  std::printf("\nAdding to %s, as %s...\n", tools.base.filename().c_str(),
              image.c_str());
  // uid 1000 ("ubuntu" in the base) runs the image and writes to these.
  // vendor/bazel-external points into this host's output base.
  std::vector<std::string> tar = {"--create",
                                  "--file",
                                  (stage / "kit.tar").string(),
                                  "--owner=1000",
                                  "--group=1000",
                                  "--numeric-owner",
                                  "--anchored",
                                  "--exclude=.arena/vendor/bazel-external",
                                  "--transform=s,^,kit/,S",
                                  "--directory",
                                  kit.string()};
  if (vendored) {
    tar.push_back(".arena/vendor");
  }
  if (cached) {
    tar.push_back(".arena/cache");
  }
  tar.insert(tar.end(), {"--directory", stage.string(), ".bazelrc.local"});
  if (RunInherit("tar", tar, kit) != 0) {
    LOG(ERROR) << "cannot archive the primed kit in " << kit;
    return 1;
  }
  return DeliverImage(
             tools, stage, {stage / "kit.tar"}, image,
             kit.parent_path() / (kit.filename().string() + ".image.tar"))
             ? 0
             : 1;
}

void PrintKitImageUsage(const std::string& image) {
  std::printf(
      "\nMade %s%s. It is anyone's: who and where are given when it runs,\n\n"
      "  docker run -it -e ARENA_SERVER=<host:port> -e ARENA_NAME=<id> -e "
      "ARENA_TOKEN=<token> %s\n\n"
      "and `arena_cli mcp` after the image is the MCP server on stdio "
      "(docker run -i).\n",
      image.c_str(), absl::GetFlag(FLAGS_push) ? " and pushed it" : "",
      image.c_str());
}

int RunKit(const ArenaRunfiles& runfiles) {
  std::filesystem::path config_path;
  const auto config = LoadConfig(&config_path);
  if (!config) {
    return 1;
  }
  const std::filesystem::path root = WorkspaceRoot();

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
  if (layered && !(absl::GetFlag(FLAGS_mint).empty() &&
                   absl::GetFlag(FLAGS_token).empty())) {
    LOG(ERROR) << "an image holds no token: hand one over with `docker run -e "
                  "ARENA_TOKEN=...`";
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
  std::vector<std::string> files =
      absl::StrSplit(EnvOr("ARENA_KIT_FILES", ""), ' ', absl::SkipWhitespace());
  for (const std::string& file : files) {
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
  // One staging dir: .arena (vendored, primed) is reused, the rest rewritten.
  const bool staged = layered && absl::GetFlag(FLAGS_out).empty();
  const std::filesystem::path out =
      staged ? StateDir(config->problem_id()) / "images" / "kit"
             : PathFlag(FLAGS_out,
                        StateDir(config->problem_id()) / "kits" /
                            (client_id.empty() ? "participant" : client_id));
  std::error_code ec;
  if (staged) {
    for (const auto& entry : std::filesystem::directory_iterator(out, ec)) {
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

  std::string token = absl::GetFlag(FLAGS_token);
  if (!client_id.empty()) {
    if (!token.empty()) {
      LOG(ERROR) << "--mint and --token are exclusive";
      return 1;
    }
    const std::filesystem::path clients = PathFlag(
        FLAGS_clients, StateDir(config->problem_id()) / "clients.textproto");
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

  const auto module = ReadFile(root / "MODULE.bazel");
  if (!module) {
    LOG(ERROR) << "no MODULE.bazel at " << root
               << "; run this from the problem's workspace";
    return 1;
  }
  if (module->find("game_arena") == std::string::npos) {
    LOG(WARNING) << "MODULE.bazel does not mention game_arena; the referee "
                    "`arena_cli spar` builds in the kit will not resolve";
  }
  if (!InstallArenaSurface(runfiles, out)) {
    return 1;
  }
  WriteFile(
      out / "MODULE.bazel",
      absl::StrCat(WithoutArenaOverride(RerootedModuleFile(*module, root)),
                   ArenaOverrideBlock()));
  for (const char* name : {"MODULE.bazel.lock", ".bazelversion", ".bazelrc"}) {
    if (const auto text = ReadFile(root / name)) {
      WriteFile(out / name, *text);
    }
  }
  for (const std::string& file : files) {
    std::filesystem::create_directories((out / file).parent_path(), ec);
    std::filesystem::copy_file(
        root / file, out / file,
        std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) {
      LOG(ERROR) << "cannot copy " << file << ": " << ec.message();
      return 1;
    }
  }

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
  // Vendored unless kits may fetch, so the kit resolves what the sandbox will.
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

  // Finds itself, so a kit that is moved still points its tools at itself.
  std::string env = absl::StrCat(
      "# . ./arena.env -- your tournament, in this shell.\n"
      "ARENA_KIT=\"$(cd \"$(dirname \"${BASH_SOURCE[0]:-$0}\")\" && pwd)\"\n"
      "export ARENA_KIT\n"
      "export PATH=\"$ARENA_KIT/.arena/bin:$PATH\"\n");
  for (const auto& [key, value] :
       {std::pair{"ARENA_SERVER", server}, std::pair{"ARENA_TOKEN", token},
        std::pair{"ARENA_NAME", client_id}}) {
    if (!value.empty()) {
      absl::StrAppend(&env, "export ", key, "=", value, "\n");
    }
  }
  WriteFile(out / "arena.env", env);
  WriteFile(out / "arena.textproto", KitConfigText(*config, server, client_id));
  if (!InstallBuiltins(runfiles, out)) {
    return 1;
  }

  const std::filesystem::path home =
      absl::GetFlag(FLAGS_kit_path).empty()
          ? out
          : std::filesystem::path(absl::GetFlag(FLAGS_kit_path));
  WriteFile(out / ".mcp.json", KitMcpJson(home, server, token, client_id));

  // arena/ is a module of its own: //... must not load its packages as ours.
  WriteFile(out / ".bazelignore",
            "# The arena is a bazel module of its own (MODULE.bazel points at\n"
            "# it); //... is your workspace, not it.\narena\n");
  WriteFile(out / ".gitignore",
            "bazel-*\n.bazelrc.local\n.arena/cache/\n.arena/bin/\n");
  const std::string arena_override = absl::GetFlag(FLAGS_arena_override);
  const std::filesystem::path cache = home / ".arena" / "cache";
  const std::filesystem::path vendor = home / ".arena" / "vendor";
  std::string local = absl::StrCat(
      home == out ? "# Written by arena_tournament kit. This host's paths; "
                    "not committed,\n# and not copied into an image of the "
                    "kit.\n"
                  : "# Written by arena_tournament kit. Yours: bazel reads it "
                    "after .bazelrc.\n",
      KitBuildSettings(home, prime, vendored));
  if (!arena_override.empty()) {
    absl::StrAppend(&local, "common --override_module=game_arena=",
                    Resolve(arena_override).string(), "\n");
  }
  // Only this tool's lines are replaced: `play` rewrites the kit every run.
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
  std::printf("  .mcp.json        arena_cli mcp, for an agent's MCP client\n");
  std::printf("  %zu file(s) from the problem\n", files.size());
  if (!token.empty()) {
    std::printf(
        "\nThe token is inside the kit; hand the directory to one "
        "participant only.\n");
  }

  if (prime) {
    // For an image, this host's rc files would change the action keys.
    const std::vector<std::string> startup = PrimeStartup(
        layered ? std::vector<std::string>{"--nohome_rc", "--nosystem_rc"}
                : std::vector<std::string>{});
    const auto bazel = [&](std::vector<std::string> command) {
      return Bazel(startup, std::move(command), out);
    };
    if (vendored) {
      std::printf("\nVendoring the kit's dependencies into %s...\n",
                  vendor.c_str());
      if (bazel({"vendor", "//..."}) != 0) {
        LOG(ERROR) << "cannot vendor the kit's dependencies";
        return 1;
      }
    }
    std::printf(
        "\nBuilding the kit once, into %s (the first time takes a while; "
        "--prime_cache=false skips it)...\n",
        cache.c_str());
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
  if (const int code = LayerKitImage(*image_tools, out, image,
                                     /*cached=*/prime, vendored);
      code != 0) {
    return code;
  }
  PrintKitImageUsage(image);
  return 0;
}

// The vendored dependencies' path in the image, without the leading /.
constexpr char kSandboxVendor[] = "opt/arena/vendor";

// Primed in the image's tree with its rc: a cache hits only the same build.
int RunSandbox(const ArenaRunfiles& runfiles) {
  std::filesystem::path config_path;
  const auto config = LoadConfig(&config_path);
  if (!config) {
    return 1;
  }
  const std::string image = config->sandbox().image();
  if (!CanPush(image, "Set sandbox.image to REGISTRY/NAME:TAG.")) {
    return 1;
  }
  const std::optional<ImageTools> tools =
      FindImageTools("", "ARENA_SANDBOX_BASE");
  if (!tools) {
    LOG(ERROR) << "the sandbox image bazel built comes with the target that "
                  "adds to it: bazel run //:sandbox_image_issue";
    return 1;
  }
  const std::filesystem::path image_rc =
      runfiles.Locate("game_arena/image/sandbox.bazelrc");
  if (image_rc.empty() || !HaveGnuTar()) {
    return 1;
  }
  if (process::ResolveExecutable("bazel").empty()) {
    LOG(ERROR) << "priming the sandbox image needs bazel on PATH";
    return 1;
  }
  const bool prime = absl::GetFlag(FLAGS_prime_cache);

  // The vendored dependencies are kept between issues; the rest is unpacked.
  const std::filesystem::path stage =
      StateDir(config->problem_id()) / "images" / "sandbox";
  const std::filesystem::path root = stage / "root";
  const std::filesystem::path workspace =
      root / std::string(sandbox_common::kWorkspace).substr(1);
  const std::filesystem::path vendor = root / kSandboxVendor;
  const std::string cache_dir =
      std::string(sandbox_common::kDiskCacheMount).substr(1);
  const std::filesystem::path scratch = stage / "image";
  std::error_code ec;
  for (const std::filesystem::path& dir :
       {workspace, root / "opt/arena/src", root / cache_dir, scratch}) {
    std::filesystem::remove_all(dir, ec);
  }
  std::filesystem::create_directories(vendor, ec);
  std::filesystem::create_directories(scratch, ec);
  const ScopedRemove cleanup{scratch};
  for (const std::string_view tar : absl::StrSplit(
           EnvOr("ARENA_SANDBOX_TARS", ""), ' ', absl::SkipWhitespace())) {
    if (RunInherit("tar",
                   {"--extract", "--file",
                    std::filesystem::absolute(std::string(tar)).string(),
                    "--directory", root.string()},
                   root) != 0) {
      LOG(ERROR) << "cannot unpack " << tar;
      return 1;
    }
  }

  // The tree leaves submissions out; the starter is the one primed with.
  std::string submission;
  const std::string& submit_dir = config->submission().files_submit_dir();
  const std::string& starter = config->kit().starter_dir();
  if (!submit_dir.empty() && !starter.empty()) {
    submission = std::filesystem::path(starter).filename().string();
    std::filesystem::create_directories(workspace / submit_dir, ec);
    std::filesystem::copy(WorkspaceRoot() / starter,
                          workspace / submit_dir / submission,
                          std::filesystem::copy_options::recursive |
                              std::filesystem::copy_options::overwrite_existing,
                          ec);
    if (ec) {
      LOG(ERROR) << "cannot copy the starter " << starter << ": "
                 << ec.message();
      return 1;
    }
  }
  // A sandbox that fetches nothing resolves the module graph from the lockfile.
  if (!std::filesystem::exists(workspace / "MODULE.bazel.lock")) {
    LOG(ERROR) << "no MODULE.bazel.lock in the sandbox's tree; run a build "
                  "and commit it";
    return 1;
  }

  const auto rc = ReadFile(image_rc);
  WriteFile(stage / "prime.bazelrc",
            tournament_arena::PrimeBazelrc(rc.value_or(""), root));
  const std::vector<std::string> startup =
      PrimeStartup({"--nohome_rc", "--nosystem_rc", "--noworkspace_rc",
                    "--bazelrc=" + (stage / "prime.bazelrc").string()});
  // A lockfile the sandbox would find stale fails here, not in the sandbox.
  std::vector<std::string> flags = {"--lockfile_mode=error",
                                    "--vendor_dir=" + vendor.string()};
  flags.insert(flags.end(), config->build().bazel_flags().begin(),
               config->build().bazel_flags().end());
  const std::vector<std::string> targets =
      tournament_arena::PrimeTargets(*config, submission);
  const auto bazel = [&](std::string_view command,
                         std::vector<std::string> extra) {
    std::vector<std::string> args = {std::string(command)};
    args.insert(args.end(), flags.begin(), flags.end());
    args.insert(args.end(), extra.begin(), extra.end());
    args.insert(args.end(), targets.begin(), targets.end());
    return Bazel(startup, std::move(args), workspace);
  };

  std::printf("\nVendoring what %s builds into %s...\n",
              absl::StrJoin(targets, " ").c_str(), vendor.c_str());
  if (bazel("vendor", {}) != 0) {
    LOG(ERROR) << "cannot vendor the sandbox's dependencies";
    return 1;
  }
  if (prime) {
    std::printf(
        "\nBuilding it once, into a fresh cache (--prime_cache=false "
        "skips it)...\n");
    // Clean first: an up-to-date output is never put in a cache. A remote hit
    // from --prime_bazelrc has to land in this one.
    if (Bazel(startup, {"clean"}, workspace) != 0 ||
        bazel("build", {"--disk_cache=" + (root / cache_dir).string(),
                        "--remote_download_outputs=all"}) != 0) {
      Bazel(startup, {"shutdown"}, workspace);
      LOG(ERROR) << "the sandbox's build fails here";
      return 1;
    }
  }
  Bazel(startup, {"shutdown"}, workspace);

  // Read-only in the image, so it points at the sandbox's /output_base already.
  const std::filesystem::path external = vendor / "bazel-external";
  std::filesystem::remove(external, ec);
  std::filesystem::create_symlink(
      std::string(sandbox_common::kOutputBaseMount) + "/external", external,
      ec);
  WriteFile(scratch / "layer" / "opt/arena/primed.bazelrc",
            absl::StrCat("# Written by sandbox_image_issue.\n"
                         "common --vendor_dir=/",
                         kSandboxVendor, "\n"));
  // Reproducible bytes, so a registry or daemon that has the layer skips it.
  std::vector<std::filesystem::path> layers = {scratch / "vendor.tar"};
  if (RunInherit("tar",
                 {"--create", "--file", layers[0].string(), "--sort=name",
                  "--mtime=@0", "--owner=0", "--group=0", "--numeric-owner",
                  "--mode=a+rX", "--directory", root.string(), kSandboxVendor,
                  "--directory", (scratch / "layer").string(),
                  "opt/arena/primed.bazelrc"},
                 scratch) != 0) {
    LOG(ERROR) << "cannot archive " << vendor;
    return 1;
  }
  // The cache is the sandbox's user's: bazel adds to it and touches what hits.
  if (prime) {
    layers.push_back(scratch / "cache.tar");
    std::vector<std::string> tar = {"--create", "--file", layers[1].string()};
    const std::vector<std::string> owner =
        tournament_arena::TarOwner(config->sandbox().run_as_user());
    tar.insert(tar.end(), owner.begin(), owner.end());
    tar.insert(tar.end(),
               {"--numeric-owner", "--directory", root.string(), cache_dir});
    if (RunInherit("tar", tar, scratch) != 0) {
      LOG(ERROR) << "cannot archive " << root / cache_dir;
      return 1;
    }
  }
  std::printf("\nAdding to %s, as %s...\n", tools->base.filename().c_str(),
              image.c_str());
  if (!DeliverImage(*tools, scratch, layers, image,
                    stage.parent_path() / "sandbox.image.tar")) {
    return 1;
  }
  std::printf(
      "\n%s %s: the sandbox image with its dependencies vendored%s.\n",
      absl::GetFlag(FLAGS_push) ? "Pushed" : "Made", image.c_str(),
      prime ? ", and a cache that fills a worker's empty cache volume" : "");
  return 0;
}

std::string KitToken(const std::filesystem::path& kit) {
  for (std::string_view line :
       absl::StrSplit(ReadFile(kit / "arena.env").value_or(""), '\n')) {
    line = absl::StripPrefix(line, "export ");
    if (absl::ConsumePrefix(&line, "ARENA_TOKEN=")) {
      return std::string(absl::StripAsciiWhitespace(line));
    }
  }
  return "";
}

std::string TailOf(const std::filesystem::path& file, int lines) {
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

// `up` and `kit` run as subprocesses: they have the flags and the checks.
int RunPlay(const ArenaRunfiles& runfiles) {
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
      PathFlag(FLAGS_data_dir, StateDir(config->problem_id()));
  const std::filesystem::path clients =
      PathFlag(FLAGS_clients, data_dir / "clients.textproto");
  const std::string client_id = absl::GetFlag(FLAGS_mint).empty()
                                    ? EnvOr("USER", "player")
                                    : absl::GetFlag(FLAGS_mint);
  const std::filesystem::path kit_dir =
      PathFlag(FLAGS_out, data_dir / "kits" / client_id);
  const int grpc_port = absl::GetFlag(FLAGS_grpc_port);
  const int http_port = absl::GetFlag(FLAGS_http_port);

  // Reuse your kit's token: minting again would be refused (one id, one entry).
  std::string token = KitToken(kit_dir);
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

  // Logs to a file: a shell over a stream of worker logs is no shell.
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
  // The pid file, not the port, which a stranger's arena may hold.
  const std::filesystem::path pid_file = data_dir / "problem_server.pid";
  std::filesystem::remove(pid_file, ec);
  auto up = process::Child::Start(self.string(), up_args,
                                  {.stdout_path = log, .stderr_path = log});
  if (!up) {
    LOG(ERROR) << "cannot start " << self;
    return 1;
  }
  std::printf("Starting the tournament (log: %s).\n", log.c_str());
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

  // Remade every run (a cached build): the image holds the problem's tree.
  const std::filesystem::path worker_bin =
      runfiles.Locate("game_arena/sandbox/worker/sandbox_worker");
  const std::vector<std::string> make_image =
      config->sandbox().allow_build_network()
          ? std::vector<std::string>{"run",
                                     EnvOr("ARENA_SANDBOX_LOAD_TARGET", "")}
          : std::vector<std::string>{"run",
                                     EnvOr("ARENA_SANDBOX_ISSUE_TARGET", ""),
                                     "--", "--prime_cache=false"};
  if (worker_bin.empty() ||
      RunInherit("bazel", make_image, WorkspaceRoot()) != 0) {
    LOG(ERROR) << "cannot make the sandbox image";
    up->Stop(std::chrono::seconds(10));
    return 1;
  }
  const std::filesystem::path worker_log = data_dir / "logs" / "worker.log";
  auto worker = process::Child::Start(
      worker_bin.string(), {absl::StrCat("--server=localhost:", grpc_port)},
      {.env = {"ARENA_WORK_DIR=" + (data_dir / "work").string()},
       .stdout_path = worker_log,
       .stderr_path = worker_log});
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
    token = KitToken(kit_dir);
  }

  struct sigaction action{};
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

  const std::string shell = absl::GetFlag(FLAGS_shell).empty()
                                ? EnvOr("SHELL", "bash")
                                : absl::GetFlag(FLAGS_shell);
  process::Options shell_options;
  shell_options.cwd = kit_dir;
  shell_options.env = {
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
  // The shell gets the terminal so Ctrl-C reaches its jobs; SIGTTOU ignored so
  // that taking it back from the background works.
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
  // The worker first: an order in flight is cancelled rather than orphaned.
  worker->Stop(std::chrono::seconds(10));
  const int status = up->Stop(std::chrono::seconds(20));
  return tournament_died ? 1 : (status == 0 ? 0 : 1);
}

void PrintUsage() {
  std::fprintf(stderr,
               "usage: arena_tournament <up|swiss|kit|sandbox|check|play> "
               "--problem_config=<path> [flags]\n"
               "  up       run the coordinator\n"
               "  swiss    re-rank every version up has seen, at /swiss\n"
               "  play     up in the background, a kit for you, a shell in it\n"
               "  kit      write a participant's workspace (--out, --mint, "
               "--image)\n"
               "  sandbox  vendor and prime the sandbox image (--push)\n"
               "  check    validate the config\n");
}

}  // namespace

int main(int argc, char** argv) {
  // Line by line, so what this prints lands before what its children do.
  std::setvbuf(stdout, nullptr, _IOLBF, 0);
  const std::vector<char*> positional = absl::ParseCommandLine(argc, argv);
  absl::InitializeLog();
  absl::SetStderrThreshold(absl::LogSeverityAtLeast::kInfo);

  if (positional.size() < 2) {
    PrintUsage();
    return 2;
  }
  const std::string command = positional[1];
  const ArenaRunfiles runfiles(argv[0]);
  const std::map<std::string, std::function<int()>> commands = {
      {"up", [&] { return RunUp(runfiles); }},
      {"swiss", [&] { return RunUp(runfiles, /*swiss=*/true); }},
      {"kit", [&] { return RunKit(runfiles); }},
      {"sandbox", [&] { return RunSandbox(runfiles); }},
      {"check", RunCheck},
      {"play", [&] { return RunPlay(runfiles); }},
  };
  if (const auto it = commands.find(command); it != commands.end()) {
    return it->second();
  }
  PrintUsage();
  return 2;
}
