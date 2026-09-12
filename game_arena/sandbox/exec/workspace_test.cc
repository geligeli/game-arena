// Preparing a tree, and the one failure that must never be recoverable.

#include "game_arena/sandbox/exec/workspace.h"

#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "gtest/gtest.h"

namespace sandbox_exec {
namespace {

class WorkspaceTest : public ::testing::Test {
 protected:
  void SetUp() override {
    root_ = std::filesystem::temp_directory_path() /
            ("workspace_test_" + std::to_string(::getpid()));
    std::filesystem::remove_all(root_);
    std::filesystem::create_directories(root_ / "logs");
    std::filesystem::create_directories(root_ / "lower");
  }
  void TearDown() override {
    std::error_code ec;
    std::filesystem::remove_all(root_, ec);
  }

  // A fake binary that logs its argv and exits |exit_code|.
  auto FakeTool(const std::string &name, int exit_code) -> std::string {
    const std::filesystem::path path = root_ / name;
    std::ofstream out(path);
    out << "#!/usr/bin/env bash\n"
        << "echo \"" << name << " $*\" >> \"" << (root_ / "tools.log").string()
        << "\"\n"
        << "echo \"" << name << " said no\" 1>&2\n"
        << "exit " << exit_code << "\n";
    out.close();
    std::filesystem::permissions(path, std::filesystem::perms::owner_all |
                                           std::filesystem::perms::group_exec |
                                           std::filesystem::perms::others_exec);
    return path.string();
  }

  auto ToolLog() const -> std::string {
    std::ifstream in(root_ / "tools.log");
    return {std::istreambuf_iterator<char>(in),
            std::istreambuf_iterator<char>()};
  }

  auto BaseWorkspace() -> proto::Workspace {
    proto::Workspace ws;
    ws.set_lower_dir((root_ / "lower").string());
    ws.set_upper_dir((root_ / "overlay").string());
    ws.set_merged_dir((root_ / "merged").string());
    ws.set_staging_dir((root_ / "staging").string());
    return ws;
  }

  std::filesystem::path root_;
};

TEST_F(WorkspaceTest, RejectsAStagedPathThatEscapes) {
  EXPECT_TRUE(IsSafeStagedPath("a/b.diff"));
  EXPECT_TRUE(IsSafeStagedPath("c-1.diff"));
  EXPECT_FALSE(IsSafeStagedPath(""));
  EXPECT_FALSE(IsSafeStagedPath("/etc/passwd"));
  EXPECT_FALSE(IsSafeStagedPath("../outside"));
  EXPECT_FALSE(IsSafeStagedPath("a/../../outside"));
  EXPECT_FALSE(IsSafeStagedPath("./here"));
}

TEST_F(WorkspaceTest, WritesStagedFilesAndRefusesAnEscapingOne) {
  proto::Workspace ws = BaseWorkspace();
  ws.set_overlay(proto::Workspace::OVERLAY_NONE);
  proto::StagedFile *file = ws.add_staged_files();
  file->set_path("c-1.diff");
  file->set_content("a patch");

  proto::Status status;
  ASSERT_TRUE(PrepareWorkspace(ws, root_ / "logs", &status))
      << status.message();
  std::ifstream in(root_ / "staging" / "c-1.diff");
  const std::string written{std::istreambuf_iterator<char>(in),
                            std::istreambuf_iterator<char>()};
  EXPECT_EQ(written, "a patch");

  ws.mutable_staged_files(0)->set_path("../escape.diff");
  proto::Status refused;
  EXPECT_FALSE(PrepareWorkspace(ws, root_ / "logs", &refused));
  EXPECT_EQ(refused.code(), proto::Status::INVALID_JOB);
}

TEST_F(WorkspaceTest, MountsTheOverlayOnTheHost) {
  proto::Workspace ws = BaseWorkspace();
  ws.set_overlay(proto::Workspace::OVERLAY_HOST);
  ws.set_mount_binary(FakeTool("mount", 0));
  ws.set_umount_binary(FakeTool("umount", 0));

  proto::Status status;
  ASSERT_TRUE(PrepareWorkspace(ws, root_ / "logs", &status))
      << status.message();

  EXPECT_NE(ToolLog().find("mount -t overlay overlay -o lowerdir=" +
                           (root_ / "lower").string() + ",upperdir=" +
                           (root_ / "overlay" / "upper").string() +
                           ",workdir=" + (root_ / "overlay" / "work").string() +
                           " " + (root_ / "merged").string()),
            std::string::npos)
      << ToolLog();
}

TEST_F(WorkspaceTest, AFailedHostOverlayFailsTheJobAndNeverDowngrades) {
  proto::Workspace ws = BaseWorkspace();
  ws.set_overlay(proto::Workspace::OVERLAY_HOST);
  ws.set_mount_binary(FakeTool("mount", 1));
  ws.set_umount_binary(FakeTool("umount", 0));

  proto::Status status;
  EXPECT_FALSE(PrepareWorkspace(ws, root_ / "logs", &status));
  EXPECT_EQ(status.code(), proto::Status::WORKSPACE_FAILED);
  // The message has to say what it costs to opt out, because the thing a
  // caller must not do is retry in the in-sandbox form.
  EXPECT_NE(status.message().find("host_overlay"), std::string::npos)
      << status.message();
  EXPECT_NE(status.message().find("CAP_SYS_ADMIN"), std::string::npos);
  // And the workspace stays in the mode it was asked for: no silent switch to
  // the privileged form.
  EXPECT_EQ(ws.overlay(), proto::Workspace::OVERLAY_HOST);
}

TEST_F(WorkspaceTest, MountListPutsTheWorkspaceRootFirst) {
  proto::Workspace ws = BaseWorkspace();
  ws.set_overlay(proto::Workspace::OVERLAY_HOST);
  proto::Mount *extra = ws.add_mounts();
  extra->set_source("/host/cache");
  extra->set_target("/disk_cache");

  const std::vector<std::string> mounts = WorkspaceMounts(ws);
  ASSERT_EQ(mounts.size(), 3u);
  EXPECT_NE(mounts[0].find("target=/workspace"), std::string::npos);
  EXPECT_NE(mounts[0].find((root_ / "merged").string()), std::string::npos);
  EXPECT_NE(mounts[1].find("target=/sandbox"), std::string::npos);
  EXPECT_NE(mounts[2].find("target=/disk_cache"), std::string::npos);
}

TEST_F(WorkspaceTest, AnInSandboxOverlayGetsTheLowerDirReadOnlyInstead) {
  proto::Workspace ws = BaseWorkspace();
  ws.set_overlay(proto::Workspace::OVERLAY_IN_SANDBOX);

  const std::vector<std::string> mounts = WorkspaceMounts(ws);
  ASSERT_GE(mounts.size(), 1u);
  EXPECT_NE(mounts[0].find("target=/repo_lower"), std::string::npos);
  EXPECT_NE(mounts[0].find("readonly"), std::string::npos);
}

TEST_F(WorkspaceTest, AppliesAHostPatchWithACheckFirst) {
  proto::Workspace ws = BaseWorkspace();
  ws.set_overlay(proto::Workspace::OVERLAY_NONE);
  ws.set_git(FakeTool("git", 0));
  ws.set_patch(proto::Workspace::PATCH_HOST);
  ws.add_patch_files("c-1.diff");
  proto::StagedFile *file = ws.add_staged_files();
  file->set_path("c-1.diff");
  file->set_content("a patch");

  proto::Status status;
  ASSERT_TRUE(PrepareWorkspace(ws, root_ / "logs", &status))
      << status.message();

  const std::string log = ToolLog();
  const auto check = log.find("git apply --check");
  const auto apply =
      log.find("git apply " + (root_ / "staging" / "c-1.diff").string());
  ASSERT_NE(check, std::string::npos) << log;
  ASSERT_NE(apply, std::string::npos) << log;
  // --check first, so a patch that does not apply says so before half of it
  // has landed.
  EXPECT_LT(check, apply);
}

TEST_F(WorkspaceTest, APatchThatDoesNotApplyIsAWorkspaceFailure) {
  proto::Workspace ws = BaseWorkspace();
  ws.set_overlay(proto::Workspace::OVERLAY_NONE);
  ws.set_git(FakeTool("git", 1));
  ws.set_patch(proto::Workspace::PATCH_HOST);
  ws.add_patch_files("c-1.diff");
  proto::StagedFile *file = ws.add_staged_files();
  file->set_path("c-1.diff");
  file->set_content("a patch");

  proto::Status status;
  EXPECT_FALSE(PrepareWorkspace(ws, root_ / "logs", &status));
  EXPECT_EQ(status.code(), proto::Status::WORKSPACE_FAILED);
  EXPECT_NE(status.message().find("does not apply"), std::string::npos)
      << status.message();
}

}  // namespace
}  // namespace sandbox_exec
