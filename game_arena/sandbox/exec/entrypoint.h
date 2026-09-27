#ifndef GAME_ARENA_GAME_ARENA_SANDBOX_EXEC_ENTRYPOINT_H
#define GAME_ARENA_GAME_ARENA_SANDBOX_EXEC_ENTRYPOINT_H

#include <string>

#include "game_arena/sandbox/exec/sandbox_job.pb.h"

namespace sandbox_exec {

// set -eu, export HOME, cd, [git apply /patches/<name>]..., [export K=V]...,
// exec <argv>. Under set -eu a patch that does not apply stops the step.
std::string EntrypointScript(const proto::Workspace &workspace,
                             const proto::Step &step);

}  // namespace sandbox_exec

#endif  // GAME_ARENA_GAME_ARENA_SANDBOX_EXEC_ENTRYPOINT_H
