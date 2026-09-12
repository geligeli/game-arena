#ifndef GAME_ARENA_GAME_ARENA_SANDBOX_EXEC_WORKSPACE_H
#define GAME_ARENA_GAME_ARENA_SANDBOX_EXEC_WORKSPACE_H

// Getting the tree ready before any step runs: clone, checkout, overlay,
// staged files, and host-side patching.
//
// The overlay is the part that carries a security claim. Assembling it here,
// on the host, is what lets a sandbox run with no capabilities at all: the
// alternative is mounting it inside, which needs CAP_SYS_ADMIN, and a sandbox
// with CAP_SYS_ADMIN running submitted build code is not a boundary. There is
// deliberately no fallback from one to the other -- a failed host mount fails
// the job, because quietly downgrading to privileged is the thing nobody
// notices.

#include <filesystem>
#include <string>
#include <vector>

#include "game_arena/sandbox/exec/sandbox_job.pb.h"

namespace sandbox_exec {

// True when |path| is safe to write under a staging directory: relative, and
// free of any "." or ".." component. The engine checks every staged path,
// even for callers that validated earlier -- it is the one writing the bytes.
auto IsSafeStagedPath(const std::string &path) -> bool;

// Clones or updates |ws.lower_dir|, writes the staged files, mounts the
// overlay when |ws| asks for one, and applies PATCH_HOST patches. Returns
// false with *status filled in.
auto PrepareWorkspace(const proto::Workspace &ws,
                      const std::filesystem::path &log_dir,
                      proto::Status *status) -> bool;

// Unmounts what PrepareWorkspace mounted. A no-op when there is nothing
// mounted, which is the state a job that failed early leaves behind.
void ReleaseWorkspace(const proto::Workspace &ws);

// The `--mount` arguments a sandbox gets for |ws|, in order: the workspace
// root first, then the scratch dir, then whatever the job asked for.
auto WorkspaceMounts(const proto::Workspace &ws) -> std::vector<std::string>;

// Where the engine itself finds |ws|'s scratch directory on the host, which
// is where a step's collect_files land.
auto ScratchDirOf(const proto::Workspace &ws) -> std::filesystem::path;

}  // namespace sandbox_exec

#endif  // GAME_ARENA_GAME_ARENA_SANDBOX_EXEC_WORKSPACE_H
