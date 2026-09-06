// Operator tooling for the arena's client registry.
/*
bazel run //game_mcts/tournament_server/tools:arena_admin -- \
    mint --client_id=some-agent --display_name="Some Agent"
*/
//
// One subcommand so far: `mint`, which prints a fresh token and the registry
// block to paste beside it.
//
// It exists because the alternative is an operator inventing their own tokens,
// and invented tokens are guessable ones. The raw token is printed once, to a
// terminal, and never written anywhere -- what goes in the registry is its
// hash, so a leaked registry file is not a set of usable credentials.

#include <cstdio>
#include <random>
#include <string>

#include "absl/flags/flag.h"
#include "absl/flags/parse.h"
#include "absl/log/globals.h"
#include "absl/log/initialize.h"
#include "game_mcts/tournament_server/server/client_registry.h"

ABSL_FLAG(std::string, client_id, "",
          "Stable id for the client; also the author recorded on everything it "
          "submits (required)");
ABSL_FLAG(std::string, display_name, "", "Human-readable name for the client");
ABSL_FLAG(int, max_active_evaluations, 0,
          "Override the problem's limit on evaluations running at once. "
          "0 uses the problem's default");
ABSL_FLAG(int, max_queued_jobs, 0,
          "Override the problem's limit on outstanding jobs. 0 uses the "
          "problem's default");

namespace {

// 256 bits from the system CSPRNG, hex encoded. random_device is the right
// source here and nowhere near a hot path.
auto MintToken() -> std::string {
  std::random_device entropy;
  std::uniform_int_distribution<unsigned> nibble(0, 15);
  static constexpr char kHex[] = "0123456789abcdef";
  std::string token;
  token.reserve(64);
  for (int i = 0; i < 64; ++i) {
    token.push_back(kHex[nibble(entropy)]);
  }
  return token;
}

void PrintUsage() {
  std::fprintf(stderr,
               "usage: arena_admin mint --client_id=<id> "
               "[--display_name=<name>]\n"
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

  const std::string token = MintToken();
  const std::string display_name = absl::GetFlag(FLAGS_display_name);

  std::printf("Token for '%s' -- shown once, store it now:\n\n  %s\n\n",
              client_id.c_str(), token.c_str());
  std::printf("The client sends it as the x-arena-token metadata header.\n\n");
  std::printf("Add this to the server's --clients file:\n\n");
  std::printf("clients {\n");
  std::printf("  client_id: \"%s\"\n", client_id.c_str());
  if (!display_name.empty()) {
    std::printf("  display_name: \"%s\"\n", display_name.c_str());
  }
  std::printf("  token_sha256: \"%s\"\n",
              tournament_arena::HashToken(token).c_str());
  const int active = absl::GetFlag(FLAGS_max_active_evaluations);
  const int queued = absl::GetFlag(FLAGS_max_queued_jobs);
  if (active > 0 || queued > 0) {
    std::printf("  quota {\n");
    if (active > 0) {
      std::printf("    max_active_evaluations: %d\n", active);
    }
    if (queued > 0) {
      std::printf("    max_queued_jobs: %d\n", queued);
    }
    std::printf("  }\n");
  }
  std::printf("}\n");
  return 0;
}
