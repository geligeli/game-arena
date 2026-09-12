// Operator tooling for the arena's client registry.
/*
bazel run //game_arena/tools:arena_admin -- \
    mint --client_id=some-agent --display_name="Some Agent"

bazel run //game_arena/tools:arena_admin -- \
    mint --client_id=some-agent --clients=/srv/arena/clients.textproto
*/
//
// One subcommand so far: `mint`, which prints a fresh token and the registry
// block to paste beside it -- or, with --clients, appends the block itself.
//
// It exists because the alternative is an operator inventing their own tokens,
// and invented tokens are guessable ones. The raw token is printed once, to a
// terminal, and never written anywhere -- what goes in the registry is its
// hash, so a leaked registry file is not a set of usable credentials.

#include <cstdio>
#include <string>

#include "absl/flags/flag.h"
#include "absl/flags/parse.h"
#include "absl/log/globals.h"
#include "absl/log/initialize.h"
#include "game_arena/server/client_registry.h"

ABSL_FLAG(std::string, client_id, "",
          "Stable id for the client; also the author recorded on everything it "
          "submits (required)");
ABSL_FLAG(std::string, display_name, "", "Human-readable name for the client");
ABSL_FLAG(std::string, clients, "",
          "Registry file to append the new client to. Empty prints the block "
          "for pasting instead");
ABSL_FLAG(int, max_active_evaluations, 0,
          "Override the problem's limit on evaluations running at once. "
          "0 uses the problem's default");
ABSL_FLAG(int, max_queued_jobs, 0,
          "Override the problem's limit on outstanding jobs. 0 uses the "
          "problem's default");

namespace {

void PrintUsage() {
  std::fprintf(stderr,
               "usage: arena_admin mint --client_id=<id> "
               "[--display_name=<name>] [--clients=<registry>]\n"
               "                        [--max_active_evaluations=N] "
               "[--max_queued_jobs=N]\n");
}

}  // namespace

auto main(int argc, char **argv) -> int {
  const std::vector<char *> positional = absl::ParseCommandLine(argc, argv);
  absl::InitializeLog();
  absl::SetStderrThreshold(absl::LogSeverityAtLeast::kWarning);

  if (positional.size() < 2 || std::string(positional[1]) != "mint") {
    PrintUsage();
    return 2;
  }
  const std::string client_id = absl::GetFlag(FLAGS_client_id);
  if (client_id.empty()) {
    std::fprintf(stderr, "--client_id is required\n");
    return 2;
  }

  tournament_arena::proto::ClientQuota quota;
  quota.set_max_active_evaluations(absl::GetFlag(FLAGS_max_active_evaluations));
  quota.set_max_queued_jobs(absl::GetFlag(FLAGS_max_queued_jobs));

  const std::string token = tournament_arena::MintToken();
  const tournament_arena::proto::Client client = tournament_arena::MakeClient(
      client_id, absl::GetFlag(FLAGS_display_name), token, quota);

  std::printf("Token for '%s' -- shown once, store it now:\n\n  %s\n\n",
              client_id.c_str(), token.c_str());
  std::printf("The client sends it as the x-arena-token metadata header.\n\n");

  const std::string registry = absl::GetFlag(FLAGS_clients);
  if (registry.empty()) {
    std::printf("Add this to the server's --clients file:\n\n%s",
                tournament_arena::ClientBlockText(client).c_str());
    return 0;
  }
  std::string error;
  if (!tournament_arena::AppendClientToRegistry(registry, client, &error)) {
    std::fprintf(stderr, "%s\n", error.c_str());
    return 1;
  }
  std::printf("Appended to %s. A running server picks it up on SIGHUP.\n",
              registry.c_str());
  return 0;
}
