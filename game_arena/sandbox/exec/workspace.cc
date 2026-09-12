#include "game_arena/sandbox/exec/workspace.h"

#include <chrono>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include "game_arena/common/process/process.h"
#include "game_arena/sandbox/common/docker.h"
#include "game_arena/sandbox/common/files.h"
#include "game_arena/sandbox/common/step.h"
#include "game_arena/sandbox/common/text.h"
#include "game_arena/sandbox/exec/checkout.h"

namespace sandbox_exec {

namespace {

using sandbox_common::ReadFile;
using sandbox_common::TailOf;

auto Fail(proto::Status *status, proto::Status::Code code,
          const std::string &message) -> bool {
  status->set_code(code);
  status->set_message(message);
  return false;
}

auto GitOf(const proto::Workspace &ws) -> std::string {
  return ws.git().empty() ? "git" : ws.git();
}

auto MountBinaryOf(const proto::Workspace &ws) -> std::string {
  return ws.mount_binary().empty() ? "mount" : ws.mount_binary();
}

auto UmountBinaryOf(const proto::Workspace &ws) -> std::string {
  return ws.umount_binary().empty() ? "umount" : ws.umount_binary();
}

auto WriteStagedFiles(const proto::Workspace &ws,
                      proto::Status *status) -> bool {
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

auto MountOverlay(const proto::Workspace &ws,
                  const std::filesystem::path &log_dir,
                  proto::Status *status) -> bool {
  // A job killed mid-flight leaves one mounted; clear it before remounting.
  ReleaseWorkspace(ws);

  std::error_code ec;
  const std::filesystem::path upper =
      std::filesystem::path(ws.upper_dir()) / "upper";
  const std::filesystem::path work =
      std::filesystem::path(ws.upper_dir()) / "work";
  std::filesystem::create_directories(upper, ec);
  std::filesystem::create_directories(work, ec);
  std::filesystem::create_directories(ws.merged_dir(), ec);
  if (ec) {
    return Fail(status, proto::Status::WORKSPACE_FAILED,
                "cannot create overlay directories: " + ec.message());
  }

  const std::string options = "lowerdir=" + ws.lower_dir() +
                              ",upperdir=" + upper.string() +
                              ",workdir=" + work.string();
  const sandbox_common::StepResult mounted = sandbox_common::RunStep(
      MountBinaryOf(ws),
      {"-t", "overlay", "overlay", "-o", options, ws.merged_dir()},
      /*cwd=*/{}, log_dir, "mount", std::chrono::seconds(60));
  if (!mounted.run.started || mounted.run.exit_code != 0) {
    // No fallback to the in-sandbox mount on purpose. That form needs
    // CAP_SYS_ADMIN, and quietly running submitted build code with it because
    // a mount failed is exactly the kind of downgrade nobody notices.
    return Fail(
        status, proto::Status::WORKSPACE_FAILED,
        "cannot mount the workspace overlay on the host: " +
            TailOf(ReadFile(log_dir / "mount.err"), 500) +
            " -- the host needs to be able to mount overlayfs (root, or a "
            "user namespace). Set sandbox.host_overlay to false to use the "
            "in-container mount instead, which requires CAP_SYS_ADMIN and is "
            "not a boundary");
  }
  return true;
}

auto ApplyHostPatches(const proto::Workspace &ws,
                      const std::filesystem::path &log_dir,
                      proto::Status *status) -> bool {
  const std::filesystem::path tree =
      ws.overlay() == proto::Workspace::OVERLAY_HOST
          ? std::filesystem::path(ws.merged_dir())
          : std::filesystem::path(ws.lower_dir());
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

auto IsSafeStagedPath(const std::string &path) -> bool {
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

auto ScratchDirOf(const proto::Workspace &ws) -> std::filesystem::path {
  return std::filesystem::path(ws.upper_dir());
}

auto PrepareWorkspace(const proto::Workspace &ws,
                      const std::filesystem::path &log_dir,
                      proto::Status *status) -> bool {
  std::error_code ec;
  std::filesystem::create_directories(log_dir, ec);

  if (!ws.source_repo().empty()) {
    std::string error;
    if (!EnsureClone(GitOf(ws), ws.source_repo(), ws.lower_dir(), log_dir,
                     &error)) {
      return Fail(status, proto::Status::WORKSPACE_FAILED, error);
    }
  }
  if (!ws.base_commit().empty()) {
    std::string error;
    if (!SyncToCommit(GitOf(ws), ws.lower_dir(), ws.base_commit(), log_dir,
                      &error)) {
      return Fail(status, proto::Status::WORKSPACE_FAILED, error);
    }
  }
  if (ws.overlay() == proto::Workspace::OVERLAY_HOST &&
      !MountOverlay(ws, log_dir, status)) {
    return false;
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

void ReleaseWorkspace(const proto::Workspace &ws) {
  if (ws.overlay() != proto::Workspace::OVERLAY_HOST ||
      ws.merged_dir().empty()) {
    return;
  }
  process::RunOptions run;
  run.timeout = std::chrono::seconds(60);
  process::RunCommand(UmountBinaryOf(ws), {ws.merged_dir()}, run);
}

auto WorkspaceMounts(const proto::Workspace &ws) -> std::vector<std::string> {
  std::vector<std::string> mounts;
  switch (ws.overlay()) {
    case proto::Workspace::OVERLAY_HOST:
      // The merge is already assembled; the sandbox just gets it.
      mounts.push_back(sandbox_common::BindMount(
          ws.merged_dir(), sandbox_common::kWorkspace, false));
      break;
    case proto::Workspace::OVERLAY_IN_SANDBOX:
      // The entrypoint assembles it, so it needs the pieces instead.
      mounts.push_back(sandbox_common::BindMount(
          ws.lower_dir(), sandbox_common::kLowerMount, true));
      break;
    case proto::Workspace::OVERLAY_NONE:
      mounts.push_back(sandbox_common::BindMount(
          ws.lower_dir(), sandbox_common::kWorkspace, false));
      break;
    default:
      break;
  }
  if (!ws.upper_dir().empty()) {
    mounts.push_back(sandbox_common::BindMount(
        ws.upper_dir(), sandbox_common::kScratch, false));
  }
  for (const proto::Mount &mount : ws.mounts()) {
    mounts.push_back(sandbox_common::BindMount(mount.source(), mount.target(),
                                               mount.readonly()));
  }
  return mounts;
}

}  // namespace sandbox_exec
