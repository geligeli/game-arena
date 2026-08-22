#ifndef RISK_GAME_AI_CPP_TOURNAMENT_SERVER_SANDBOX_WORKER_CANDIDATE_BUILD_H
#define RISK_GAME_AI_CPP_TOURNAMENT_SERVER_SANDBOX_WORKER_CANDIDATE_BUILD_H

// Generates the BUILD file a submitted candidate is compiled with.
//
// Pure string generation, kept apart from the backend that writes and runs it
// so the exact bazel a submission produces can be asserted in a unit test
// rather than only observed through a build failure.

#include <string>
#include <vector>

#include "cpp/tournament_server/arena.pb.h"

namespace tournament_arena {

// Where a candidate is patched into the repo clone, relative to the workspace
// root. The generated label is //<kCandidateDir>/<candidate_id>:bot.
inline constexpr const char *kCandidateDir = "cpp/tournament_server/candidates";

// The bazel label of |order|'s bot binary.
auto CandidateTarget(const std::string &candidate_id) -> std::string;

// The BUILD file contents for |order|. Returns an empty string when the order
// is unusable (no files, or an entry header that is not one of them) -- the
// same conditions the arena rejects at submit time, re-checked because the
// worker must never write a BUILD naming a file it did not receive.
auto GenerateCandidateBuild(const proto::WorkOrder &order) -> std::string;

// The game-selection define for a registry key. Empty for an unknown game.
auto CandidateGameDefine(const std::string &game) -> std::string;

// "a=1,b=2" from the order's params, in a stable order so a rebuilt candidate
// gets a byte-identical command line.
auto FormatParams(const proto::WorkOrder &order) -> std::string;

}  // namespace tournament_arena

#endif  // RISK_GAME_AI_CPP_TOURNAMENT_SERVER_SANDBOX_WORKER_CANDIDATE_BUILD_H
