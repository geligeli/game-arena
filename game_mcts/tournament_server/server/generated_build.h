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

#include "game_mcts/tournament_server/proto/problem.pb.h"

namespace tournament_arena {

// The bazel label of a structured submission's bot binary.
auto CandidateTarget(const std::string &dir,
                     const std::string &candidate_id) -> std::string;

// The BUILD file contents. Returns an empty string when the submission is
// unusable: no files, an entry header that is not one of them, or a harness
// that does not say what to compile against.
//
// |harness| supplies every label that ends up in the generated BUILD. It comes
// from the problem config rather than from here on purpose: which library a
// solution links is the problem's business, not the arena's.
auto GenerateCandidateBuild(
    const std::string &dir, const std::string &candidate_id,
    const proto::CandidateHarness &harness,
    const std::vector<std::string> &file_paths, const std::string &entry_header,
    const std::vector<std::string> &extra_deps) -> std::string;

}  // namespace tournament_arena

#endif  // GAME_MCTS_GAME_MCTS_TOURNAMENT_SERVER_SERVER_GENERATED_BUILD_H
