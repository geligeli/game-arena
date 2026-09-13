#include "game_arena/sandbox/exec/workspace.h"

#include <chrono>
#include <filesystem>
#include <string>
#include <system_error>

#include "game_arena/sandbox/common/files.h"
#include "game_arena/sandbox/common/step.h"
#include "game_arena/sandbox/common/text.h"
#include "game_arena/sandbox/exec/checkout.h"

namespace sandbox_exec {

namespace {

using sandbox_common::TailOf;

bool Fail(proto::Status *status, proto::Status::Code code,
          const std::string &message) {
  status->set_code(code);
  status->set_message(message);
  return false;
}

std::string GitOf(const proto::Workspace &ws) {
  return ws.git().empty() ? "git" : ws.git();
}

std::string TarOf(const proto::Workspace &ws) {
  return ws.tar().empty() ? "tar" : ws.tar();
}

bool WriteStagedFiles(const proto::Workspace &ws, proto::Status *status) {
  if (ws.staging_dir().empty()) {
    return true;
  }
  const std::filesystem::path staging(ws.staging_dir());
  std::error_code ec;
  std::filesystem::remove_all(staging, ec);
  std::filesystem::create_directories(staging, ec);
  if (ec) {
    return Fail(status, proto::Status::WORKSPACE_FAILED,
                "cannot create the staging directory: " + ec.message());
  }
  for (const proto::StagedFile &file : ws.staged_files()) {
    if (!IsSafeStagedPath(file.path())) {
      return Fail(status, proto::Status::INVALID_JOB,
                  "staged path escapes the staging directory: " + file.path());
    }
    std::string error;
    if (!sandbox_common::WriteFile(staging / file.path(), file.content(),
                                   &error)) {
      return Fail(status, proto::Status::WORKSPACE_FAILED, error);
    }
  }
  return true;
}

bool ApplyHostPatches(const proto::Workspace &ws,
                      const std::filesystem::path &log_dir,
                      proto::Status *status) {
  const std::filesystem::path tree(ws.tree_dir());
  for (const std::string &name : ws.patch_files()) {
    const std::filesystem::path diff =
        std::filesystem::path(ws.staging_dir()) / name;
    // --check first, so a patch that does not apply says so before half of it
    // has landed.
    const sandbox_common::StepResult check = sandbox_common::RunStep(
        GitOf(ws), {"apply", "--check", diff.string()}, tree, log_dir,
        "apply_check_" + name, std::chrono::seconds(120));
    if (!check.run.started) {
      return Fail(status, proto::Status::TOOL_MISSING,
                  "cannot run git ('" + GitOf(ws) + "' not found)");
    }
    if (check.run.exit_code != 0) {
      return Fail(
          status, proto::Status::WORKSPACE_FAILED,
          name + " does not apply to this tree: " + TailOf(check.output, 1500));
    }
    const sandbox_common::StepResult apply = sandbox_common::RunStep(
        GitOf(ws), {"apply", diff.string()}, tree, log_dir, "apply_" + name,
        std::chrono::seconds(120));
    if (!apply.run.started || apply.run.exit_code != 0) {
      return Fail(status, proto::Status::WORKSPACE_FAILED,
                  "cannot apply " + name + ": " + TailOf(apply.output, 1500));
    }
  }
  return true;
}

}  // namespace

bool IsSafeStagedPath(const std::string &path) {
  if (path.empty() || path.front() == '/') {
    return false;
  }
  for (const std::filesystem::path &part : std::filesystem::path(path)) {
    if (part == ".." || part == ".") {
      return false;
    }
  }
  return true;
}

bool PrepareWorkspace(const proto::Workspace &ws,
                      const std::filesystem::path &log_dir,
                      proto::Status *status) {
  std::error_code ec;
  std::filesystem::create_directories(log_dir, ec);

  if (!ws.source_repo().empty()) {
    std::string error;
    if (!EnsureClone(GitOf(ws), ws.source_repo(), ws.tree_dir(), log_dir,
                     &error)) {
      return Fail(status, proto::Status::WORKSPACE_FAILED, error);
    }
  }
  if (!ws.base_commit().empty()) {
    std::string error;
    if (!SyncToCommit(GitOf(ws), ws.tree_dir(), ws.base_commit(), log_dir,
                      &error)) {
      return Fail(status, proto::Status::WORKSPACE_FAILED, error);
    }
  }
  // Where a process engine's steps collect files from; a container engine
  // keeps its scratch in a volume and leaves this empty.
  if (!ws.scratch_dir().empty()) {
    std::filesystem::create_directories(ws.scratch_dir(), ec);
  }
  if (!WriteStagedFiles(ws, status)) {
    return false;
  }
  if (ws.patch() == proto::Workspace::PATCH_HOST &&
      !ApplyHostPatches(ws, log_dir, status)) {
    return false;
  }
  return true;
}

bool ExportTree(const proto::Workspace &ws,
                const std::filesystem::path &archive,
                const std::filesystem::path &log_dir, proto::Status *status) {
  std::error_code ec;
  std::filesystem::create_directories(archive.parent_path(), ec);
  // Without .git: the sandbox builds a tree, it does not need the history,
  // and for a --local clone the objects are hardlinks into the source repo.
  const sandbox_common::StepResult exported = sandbox_common::RunStep(
      TarOf(ws),
      {"--exclude=./.git", "-cf", archive.string(), "-C", ws.tree_dir(), "."},
      /*cwd=*/{}, log_dir, "export", std::chrono::seconds(600));
  if (!exported.run.started) {
    return Fail(status, proto::Status::TOOL_MISSING,
                "cannot run tar ('" + TarOf(ws) + "' not found)");
  }
  if (exported.run.exit_code != 0) {
    return Fail(status, proto::Status::WORKSPACE_FAILED,
                "cannot export the tree: " + TailOf(exported.output, 1000));
  }
  return true;
}

}  // namespace sandbox_exec
