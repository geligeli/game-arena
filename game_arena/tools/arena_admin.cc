// Operator tooling for the arena's client registry.
/*
bazel run //game_arena/tools:arena_admin -- \
    mint --client_id=some-agent --display_name="Some Agent"

bazel run //game_arena/tools:arena_admin -- \
    mint --client_id=some-agent --clients=/srv/arena/clients.textproto
*/
//
// One subcommand so far: `mint`, which prints a fresh token and the registry
// block to paste beside it -- or, with --clients, appends the block itself, or
// with --overwrite gives a client already there a new token.
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
          "the rest of its entry. Its old token stops working");
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
      "Prints the token once. With --clients the client is added to that\n"
      "file, or with --overwrite given a new token there; without it, the\n"
      "block to paste is printed.");
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
  const bool overwrite = absl::GetFlag(FLAGS_overwrite);
  if (overwrite ? !tournament_arena::ReplaceClientToken(
                      registry, client_id, client.token_sha256(), &error)
                : !tournament_arena::AppendClientToRegistry(registry, client,
                                                            &error)) {
    std::fprintf(stderr, "%s\n", error.c_str());
    return 1;
  }
  std::printf(
      "%s %s. A running server reads it on the token's first use.\n",
      overwrite ? "Replaced the token in" : "Appended to", registry.c_str());
  return 0;
}
