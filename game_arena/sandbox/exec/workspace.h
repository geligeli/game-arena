#ifndef GAME_ARENA_GAME_ARENA_SANDBOX_EXEC_WORKSPACE_H
#define GAME_ARENA_GAME_ARENA_SANDBOX_EXEC_WORKSPACE_H

// Getting the tree ready before any step runs: clone, checkout, staged files,
// host-side patching, and exporting the tree for a container engine to load.
//
// Nothing here is mounted anywhere. A container engine copies the exported
// tree into a volume through the daemon, which is what lets a worker be
// "anything with a docker socket": no host path has to exist on both sides,
// no overlay has to be assembled, and no sandbox needs a capability to
// assemble one.

#include <filesystem>
#include <string>

#include "game_arena/sandbox/exec/sandbox_job.pb.h"

namespace sandbox_exec {

// True when |path| is safe to write under a staging directory: relative, and
// free of any "." or ".." component. The engine checks every staged path,
// even for callers that validated earlier -- it is the one writing the bytes.
bool IsSafeStagedPath(const std::string &path);

// Clones or updates |ws.tree_dir|, creates the scratch dir, writes the staged
// files, and applies PATCH_HOST patches. Returns false with *status filled in.
bool PrepareWorkspace(const proto::Workspace &ws,
                      const std::filesystem::path &log_dir,
                      proto::Status *status);

// Writes |ws.tree_dir| as a tar archive at |archive|, without its .git: what
// a container engine loads into the sandbox's workspace. Returns false with
// *status filled in.
bool ExportTree(const proto::Workspace &ws,
                const std::filesystem::path &archive,
                const std::filesystem::path &log_dir, proto::Status *status);

}  // namespace sandbox_exec

#endif  // GAME_ARENA_GAME_ARENA_SANDBOX_EXEC_WORKSPACE_H
