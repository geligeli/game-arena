#ifndef GAME_ARENA_GAME_ARENA_SANDBOX_EXEC_WORKSPACE_H
#define GAME_ARENA_GAME_ARENA_SANDBOX_EXEC_WORKSPACE_H

#include <filesystem>
#include <string>

#include "game_arena/sandbox/exec/sandbox_job.pb.h"

namespace sandbox_exec {

// Relative, with no "." or ".." part. Checked here even if a caller did.
bool IsSafeStagedPath(const std::string &path);

// Creates the dirs, writes the staged files and applies PATCH_HOST patches.
bool PrepareWorkspace(const proto::Workspace &ws,
                      const std::filesystem::path &log_dir,
                      proto::Status *status);

}  // namespace sandbox_exec

#endif  // GAME_ARENA_GAME_ARENA_SANDBOX_EXEC_WORKSPACE_H
