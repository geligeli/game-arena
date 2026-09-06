#ifndef GAME_MCTS_GAME_MCTS_TOURNAMENT_SERVER_SERVER_GENERATED_BUILD_H
#define GAME_MCTS_GAME_MCTS_TOURNAMENT_SERVER_SERVER_GENERATED_BUILD_H

// The BUILD file a structured submission is compiled with.
//
// Runs at submit time, on the coordinator, because the generated BUILD is part
// of the patch that gets stored. That is what keeps one execution path: a
// structured submission and a hand-written patch are the same thing by the
// time anything downstream sees them, and the worker only ever runs
// `git apply`.
//
// Generating it rather than accepting one is also what makes the dependency
// allowlist enforceable. A submitter who could write their own BUILD could
// write a genrule, and a genrule runs arbitrary code at build time. A problem
// that allows patches to touch BUILD files has given that up deliberately and
// must rely on the sandbox instead (see SandboxSpec).
//
// Pure string generation, kept apart from everything that writes or runs it so
// the exact bazel a submission produces can be asserted in a unit test rather
// than only observed through a build failure.

#include <string>
#include <vector>

namespace tournament_arena {

// Where a structured submission is patched into the repo, relative to the
// workspace root. The generated target is //<dir>/<candidate_id>:bot.
inline constexpr const char *kDefaultCandidateDir =
    "game_mcts/tournament_server/candidates";

// The bazel label of a structured submission's bot binary.
auto CandidateTarget(const std::string &dir,
                     const std::string &candidate_id) -> std::string;

// The BUILD file contents. Returns an empty string when the submission is
// unusable: no files, an entry header that is not one of them, or a game the
// candidate harness cannot be compiled for.
auto GenerateCandidateBuild(
    const std::string &dir, const std::string &candidate_id,
    const std::string &game, const std::vector<std::string> &file_paths,
    const std::string &entry_header,
    const std::vector<std::string> &extra_deps) -> std::string;

// The game-selection define for a registry key. Empty for an unknown game.
auto CandidateGameDefine(const std::string &game) -> std::string;

}  // namespace tournament_arena

#endif  // GAME_MCTS_GAME_MCTS_TOURNAMENT_SERVER_SERVER_GENERATED_BUILD_H
