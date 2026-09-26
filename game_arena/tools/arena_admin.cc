// Operator tooling for the arena's client registry.
/*
bazel run //game_arena/tools:arena_admin -- \
    mint --client_id=some-agent --display_name="Some Agent"

bazel run //game_arena/tools:arena_admin -- \
    mint --client_id=some-agent --clients=/srv/arena/clients.textproto
*/
//
// One subcommand so far: `mint`, which prints a fresh token -- and nothing else
// on stdout, so TOKEN=$(arena_admin mint ...) works -- and, with --clients,
// writes the client to that file. Without --clients the registry block to
// paste goes to stderr.
//
// It exists because the alternative is an operator inventing their own tokens,
// and invented tokens are guessable ones. The raw token is printed once, to a
// terminal, and never written anywhere -- what goes in the registry is its
// hash, so a leaked registry file is not a set of usable credentials.

#include <cstdio>
#include <string>

#include "absl/flags/flag.h"
#include "absl/flags/parse.h"
#include "absl/flags/usage.h"
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
ABSL_FLAG(bool, overwrite, false,
          "With --clients: give a client already there a new token, keeping "
          "the rest of its entry, and add one that is not. Its old token "
          "stops working");
ABSL_FLAG(int, max_active_evaluations, 0,
          "Override the problem's limit on evaluations running at once. "
          "0 uses the problem's default");
ABSL_FLAG(int, max_queued_jobs, 0,
          "Override the problem's limit on outstanding jobs. 0 uses the "
          "problem's default");

int main(int argc, char **argv) {
  absl::SetProgramUsageMessage(
      "mints a client token for the arena's client registry.\n\n"
      "  arena_admin mint --client_id=<id> [--display_name=<name>]\n"
      "      [--clients=<registry> [--overwrite]]\n"
      "      [--max_active_evaluations=N] [--max_queued_jobs=N]\n\n"
      "Prints the token, and only the token, on stdout. With --clients the\n"
      "client is added to that file, or with --overwrite given a new token\n"
      "there whether or not it was; without it, the block to paste goes to\n"
      "stderr.");
  const std::vector<char *> positional = absl::ParseCommandLine(argc, argv);
  absl::InitializeLog();
  absl::SetStderrThreshold(absl::LogSeverityAtLeast::kWarning);

  if (positional.size() < 2 || std::string(positional[1]) != "mint") {
    std::fprintf(stderr, "%s\n", absl::ProgramUsageMessage().data());
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

  const std::string registry = absl::GetFlag(FLAGS_clients);
  if (registry.empty()) {
    std::fprintf(stderr, "Add this to the server's --clients file:\n\n%s",
                 tournament_arena::ClientBlockText(client).c_str());
  } else {
    std::string error;
    if (absl::GetFlag(FLAGS_overwrite)
            ? !tournament_arena::SetClientToken(registry, client, &error)
            : !tournament_arena::AppendClientToRegistry(registry, client,
                                                        &error)) {
      std::fprintf(stderr, "%s\n", error.c_str());
      return 1;
    }
  }
  std::printf("%s\n", token.c_str());
  return 0;
}
