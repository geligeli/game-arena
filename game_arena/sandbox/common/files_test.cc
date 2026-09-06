#include "game_arena/sandbox/common/files.h"

#include <gtest/gtest.h>
#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <string>

namespace sandbox_common {
namespace {

class FilesTest : public ::testing::Test {
 protected:
  void SetUp() override {
    dir_ = std::filesystem::temp_directory_path() /
           ("sandbox_files_test_" + std::to_string(::getpid()));
    std::filesystem::create_directories(dir_);
  }
  void TearDown() override {
    std::error_code ec;
    std::filesystem::remove_all(dir_, ec);
  }
  std::filesystem::path dir_;
};

TEST_F(FilesTest, ReadFileReturnsEmptyForMissingFile) {
  EXPECT_EQ(ReadFile(dir_ / "absent"), "");
}

TEST_F(FilesTest, WriteFileCreatesParentsAndRoundTrips) {
  const std::filesystem::path file = dir_ / "a" / "b" / "patch.diff";
  std::string error;
  ASSERT_TRUE(WriteFile(file, "line one\nline two\n", &error)) << error;
  EXPECT_EQ(ReadFile(file), "line one\nline two\n");
}

TEST_F(FilesTest, WriteFileTruncatesExistingContent) {
  const std::filesystem::path file = dir_ / "f";
  std::string error;
  ASSERT_TRUE(WriteFile(file, "long old content", &error)) << error;
  ASSERT_TRUE(WriteFile(file, "new", &error)) << error;
  EXPECT_EQ(ReadFile(file), "new");
}

TEST_F(FilesTest, WriteFileFailsWhenParentIsAFile) {
  const std::filesystem::path blocker = dir_ / "blocker";
  { std::ofstream(blocker) << "x"; }
  std::string error;
  EXPECT_FALSE(WriteFile(blocker / "child", "content", &error));
  EXPECT_FALSE(error.empty());
}

}  // namespace
}  // namespace sandbox_common
