// Manual client for the sandbox runner: sends one Run (or one Kill) to a
// running sandbox_runner server so a task can be tried out by hand.
/*
bazel run //game_mcts/tournament_server/sandbox/runner:sandbox_cli -- \
  --server=localhost:50052 --target=//game_mcts/games/risk:risk_main \
  -- --seed=7 --games=3
*/
//
// Everything past the trailing `--` -- the second one in the form above, the
// first belonging to `bazel run` -- is forwarded to the target as its own
// argv. Patches (the files a real request would carry) come from --patch_dir,
// whose contents are sent keyed by their path relative to that directory, and
// from --patch entries naming individual files.
//
// The run's stdout and stderr are replayed on this process' stdout and
// stderr, and the target's exit code becomes this process' exit code. Ctrl-C
// sends Kill for the in-flight identifier instead of just dropping the RPC,
// so the container on the server actually stops; a second Ctrl-C aborts.

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <grpcpp/grpcpp.h>
#include <unistd.h>

#include "absl/flags/flag.h"
#include "absl/flags/parse.h"
#include "absl/log/globals.h"
#include "absl/log/initialize.h"
#include "absl/log/log.h"
#include "game_mcts/tournament_server/proto/sandbox_runner.grpc.pb.h"

ABSL_FLAG(std::string, server, "localhost:50052",
          "host:port of the sandbox runner");
ABSL_FLAG(std::string, target, "",
          "Bazel target to run in the sandbox, e.g. //game_mcts/core/mcts:mcts "
          "(required unless --kill)");
ABSL_FLAG(std::string, identifier, "",
          "Run token, also the handle Kill uses. Empty: cli-<pid>-<epoch_s>");
ABSL_FLAG(std::string, patch_dir, "",
          "Every file below this directory is sent as a patch, keyed by its "
          "path relative to the directory");
ABSL_FLAG(std::string, patch, "",
          "Extra patches as a comma-separated list of <repo/path>=<host file>, "
          "or plain <repo/path> to read the file at that path under the "
          "current directory");
ABSL_FLAG(int, deadline_s, 0,
          "Client-side RPC deadline; 0 waits forever (the server enforces its "
          "own --timeout_s)");
ABSL_FLAG(std::string, kill, "",
          "Send Kill for this identifier and exit, running nothing");

namespace {

namespace proto = tournament_broker::proto;

// Polled by the killer thread rather than signalled through a condition
// variable, since locking a mutex in signal context is not safe.
std::atomic<bool> g_interrupted{false};

// Runs in signal context, so it only restores the default handler -- a second
// Ctrl-C then kills this process even if the Kill RPC hangs -- and raises the
// flag the killer thread is watching.
extern "C" void OnInterrupt(int signum) {
  std::signal(signum, SIG_DFL);
  g_interrupted.store(true);
}

auto DefaultIdentifier() -> std::string {
  const auto now = std::chrono::system_clock::now().time_since_epoch();
  return "cli-" + std::to_string(::getpid()) + "-" +
         std::to_string(
             std::chrono::duration_cast<std::chrono::seconds>(now).count());
}

auto ApplyDeadline(grpc::ClientContext *context) -> void {
  const int deadline_s = absl::GetFlag(FLAGS_deadline_s);
  if (deadline_s > 0) {
    context->set_deadline(std::chrono::system_clock::now() +
                          std::chrono::seconds(deadline_s));
  }
}

auto ReadFile(const std::filesystem::path &path, std::string *content) -> bool {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return false;
  }
  *content = std::string(std::istreambuf_iterator<char>(in),
                         std::istreambuf_iterator<char>());
  return in.good() || in.eof();
}

// --patch_dir: send the whole tree, keyed by path relative to the dir, which
// is exactly the layout the runner copies onto the workspace.
auto AddPatchDir(const std::filesystem::path &dir,
                 proto::RunSandboxRequest *request) -> bool {
  std::error_code ec;
  if (!std::filesystem::is_directory(dir, ec)) {
    LOG(ERROR) << "--patch_dir " << dir << " is not a directory";
    return false;
  }
  for (const auto &entry :
       std::filesystem::recursive_directory_iterator(dir, ec)) {
    if (!entry.is_regular_file()) {
      continue;
    }
    const std::string key =
        entry.path().lexically_relative(dir).generic_string();
    std::string content;
    if (!ReadFile(entry.path(), &content)) {
      LOG(ERROR) << "Cannot read " << entry.path();
      return false;
    }
    (*request->mutable_patches())[key] = std::move(content);
  }
  if (ec) {
    LOG(ERROR) << "Cannot walk --patch_dir " << dir << ": " << ec.message();
    return false;
  }
  return true;
}

// --patch: comma-separated <repo/path>=<host file>, or bare <repo/path> when
// the host file sits at the same relative path under the current directory.
auto AddPatchFlag(const std::string &spec, proto::RunSandboxRequest *request)
    -> bool {
  for (std::string::size_type pos = 0; pos < spec.size();) {
    const std::string::size_type comma = spec.find(',', pos);
    const std::string entry = spec.substr(pos, comma - pos);
    pos = comma == std::string::npos ? spec.size() : comma + 1;
    if (entry.empty()) {
      continue;
    }
    const std::string::size_type eq = entry.find('=');
    const std::string key =
        eq == std::string::npos ? entry : entry.substr(0, eq);
    const std::string source =
        eq == std::string::npos ? entry : entry.substr(eq + 1);
    std::string content;
    if (!ReadFile(source, &content)) {
      LOG(ERROR) << "Cannot read patch source '" << source << "'";
      return false;
    }
    (*request->mutable_patches())[key] = std::move(content);
  }
  return true;
}

auto WriteAll(std::FILE *stream, const std::string &data) -> void {
  if (!data.empty()) {
    std::fwrite(data.data(), 1, data.size(), stream);
  }
  std::fflush(stream);
}

auto DoKill(proto::SandboxService::Stub *stub, const std::string &identifier)
    -> int {
  grpc::ClientContext context;
  ApplyDeadline(&context);
  proto::KillRequest request;
  request.set_identifier(identifier);
  proto::KillResponse response;
  const grpc::Status status = stub->Kill(&context, request, &response);
  if (!status.ok()) {
    LOG(ERROR) << "Kill failed: " << status.error_message() << " (code "
               << status.error_code() << ")";
    return 1;
  }
  LOG(INFO) << "Killed " << identifier;
  return 0;
}

}  // namespace

auto main(int argc, char **argv) -> int {
  const std::vector<char *> positional = absl::ParseCommandLine(argc, argv);
  absl::InitializeLog();
  absl::SetStderrThreshold(absl::LogSeverityAtLeast::kInfo);

  const auto channel = grpc::CreateChannel(absl::GetFlag(FLAGS_server),
                                           grpc::InsecureChannelCredentials());
  const auto stub = proto::SandboxService::NewStub(channel);

  if (!absl::GetFlag(FLAGS_kill).empty()) {
    return DoKill(stub.get(), absl::GetFlag(FLAGS_kill));
  }

  if (absl::GetFlag(FLAGS_target).empty()) {
    LOG(ERROR) << "Missing required --target=//path/to:target";
    return 2;
  }

  proto::RunSandboxRequest request;
  request.set_bazel_target(absl::GetFlag(FLAGS_target));
  const std::string identifier = absl::GetFlag(FLAGS_identifier).empty()
                                     ? DefaultIdentifier()
                                     : absl::GetFlag(FLAGS_identifier);
  request.set_identifier(identifier);
  // Everything absl left unparsed (i.e. after `--`) is the target's own argv.
  for (std::size_t i = 1; i < positional.size(); ++i) {
    request.add_args(positional[i]);
  }
  if (!absl::GetFlag(FLAGS_patch_dir).empty() &&
      !AddPatchDir(absl::GetFlag(FLAGS_patch_dir), &request)) {
    return 1;
  }
  if (!absl::GetFlag(FLAGS_patch).empty() &&
      !AddPatchFlag(absl::GetFlag(FLAGS_patch), &request)) {
    return 1;
  }

  LOG(INFO) << "run " << identifier << " on " << absl::GetFlag(FLAGS_server)
            << ": bazel run " << request.bazel_target() << " ("
            << request.args_size() << " arg(s), " << request.patches_size()
            << " patch file(s)); Ctrl-C kills the run";

  // The Run RPC blocks until the container exits, so the Kill on Ctrl-C has
  // to come from a second thread; it also unblocks Run, which then returns.
  std::atomic<bool> done{false};
  std::signal(SIGINT, OnInterrupt);
  std::thread killer([&] {
    while (!done.load() && !g_interrupted.load()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (!done.load()) {
      LOG(WARNING) << "Interrupted; killing " << identifier;
      DoKill(stub.get(), identifier);
    }
  });

  grpc::ClientContext context;
  ApplyDeadline(&context);
  proto::RunSandboxResponse response;
  const grpc::Status status = stub->Run(&context, request, &response);

  done.store(true);
  killer.join();

  if (!status.ok()) {
    LOG(ERROR) << "Run failed: " << status.error_message() << " (code "
               << status.error_code() << ")";
    return 1;
  }
  WriteAll(stdout, response.stdout());
  WriteAll(stderr, response.stderr());
  LOG(INFO) << "run " << identifier << ": exit " << response.exit_code();
  return response.exit_code();
}
