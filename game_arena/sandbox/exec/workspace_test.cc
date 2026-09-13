// Preparing a tree, and exporting it for a container to load.

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
  std::string FakeTool(const std::string &name, int exit_code) {
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

  std::string ToolLog() const {
    std::ifstream in(root_ / "tools.log");
    return {std::istreambuf_iterator<char>(in),
            std::istreambuf_iterator<char>()};
  }

  proto::Workspace BaseWorkspace() {
    proto::Workspace ws;
    ws.set_tree_dir((root_ / "lower").string());
    ws.set_scratch_dir((root_ / "scratch").string());
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

TEST_F(WorkspaceTest, HeadMeansTheSourcesTipNotTheClones) {
  proto::Workspace ws = BaseWorkspace();
  ws.set_git(FakeTool("git", 0));
  ws.set_base_commit("HEAD");

  proto::Status status;
  ASSERT_TRUE(PrepareWorkspace(ws, root_ / "logs", &status))
      << status.message();
  // A clone's own HEAD never moves; after the fetch, the tip is origin/HEAD.
  EXPECT_NE(ToolLog().find("git checkout --force origin/HEAD"),
            std::string::npos)
      << ToolLog();
  EXPECT_NE(ToolLog().find("git clean -fdx"), std::string::npos);
}

TEST_F(WorkspaceTest, CreatesTheScratchDir) {
  proto::Workspace ws = BaseWorkspace();
  proto::Status status;
  ASSERT_TRUE(PrepareWorkspace(ws, root_ / "logs", &status))
      << status.message();
  EXPECT_TRUE(std::filesystem::is_directory(root_ / "scratch"));
}

TEST_F(WorkspaceTest, ExportsTheTreeWithoutItsGitDir) {
  proto::Workspace ws = BaseWorkspace();
  ws.set_tar(FakeTool("tar", 0));

  proto::Status status;
  ASSERT_TRUE(
      ExportTree(ws, root_ / "out" / "tree.tar", root_ / "logs", &status))
      << status.message();
  EXPECT_NE(ToolLog().find("tar --exclude=./.git -cf " +
                           (root_ / "out" / "tree.tar").string() + " -C " +
                           (root_ / "lower").string() + " ."),
            std::string::npos)
      << ToolLog();
}

TEST_F(WorkspaceTest, AFailedExportIsAWorkspaceFailure) {
  proto::Workspace ws = BaseWorkspace();
  ws.set_tar(FakeTool("tar", 1));

  proto::Status status;
  EXPECT_FALSE(
      ExportTree(ws, root_ / "out" / "tree.tar", root_ / "logs", &status));
  EXPECT_EQ(status.code(), proto::Status::WORKSPACE_FAILED);
  EXPECT_NE(status.message().find("tar said no"), std::string::npos)
      << status.message();
}

TEST_F(WorkspaceTest, ARealTarExportsARealTree) {
  std::ofstream(root_ / "lower" / "hello.txt") << "hi";
  std::filesystem::create_directories(root_ / "lower" / ".git");
  std::ofstream(root_ / "lower" / ".git" / "HEAD") << "ref";
  proto::Workspace ws = BaseWorkspace();

  proto::Status status;
  ASSERT_TRUE(ExportTree(ws, root_ / "tree.tar", root_ / "logs", &status))
      << status.message();
  std::ifstream in(root_ / "tree.tar", std::ios::binary);
  const std::string archive{std::istreambuf_iterator<char>(in),
                            std::istreambuf_iterator<char>()};
  EXPECT_NE(archive.find("hello.txt"), std::string::npos);
  EXPECT_EQ(archive.find(".git/HEAD"), std::string::npos);
}

TEST_F(WorkspaceTest, AppliesAHostPatchWithACheckFirst) {
  proto::Workspace ws = BaseWorkspace();
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
