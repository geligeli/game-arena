#ifndef GAME_ARENA_GAME_ARENA_SANDBOX_EXEC_ENTRYPOINT_H
#define GAME_ARENA_GAME_ARENA_SANDBOX_EXEC_ENTRYPOINT_H

// The shell script a container is handed as its entrypoint.
//
// One builder, where there were four hand-assembled copies of the same
// skeleton -- build, run, grade, and the standalone runner's -- each spelling
// `set -eu`, the overlay prelude and the `cd` slightly differently. What
// differs between them is data: which prelude, whether the staged files are
// patched or copied, and the command line.

#include <string>

#include "game_arena/sandbox/exec/sandbox_job.pb.h"

namespace sandbox_exec {

// The script for |step| in |workspace|.
//
//   set -eu                      a patch that does not apply aborts here,
//                                with git's own message in the log, rather
//                                than becoming a confusing error later
//   export HOME=/sandbox         the one thing left to arrange
//   [cp -a /patches/. /workspace/]
//   cd <workspace root>
//   [git apply '/patches/<name>'] ...
//   [export KEY='value'] ...
//   exec <argv>
std::string EntrypointScript(const proto::Workspace &workspace,
                             const proto::Step &step);

// |argv| rendered for a shell: verbatim tokens as they are, everything else
// single-quoted.
std::string RenderArgv(const proto::Step &step);

}  // namespace sandbox_exec

#endif  // GAME_ARENA_GAME_ARENA_SANDBOX_EXEC_ENTRYPOINT_H
