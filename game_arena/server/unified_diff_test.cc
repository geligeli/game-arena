#include "game_arena/server/unified_diff.h"

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

namespace tournament_arena {
namespace {

auto Parse(const std::string &diff, Patch *patch) -> bool {
  std::string error;
  const bool ok = ParseUnifiedDiff(diff, patch, &error);
  if (!ok) {
    ADD_FAILURE() << "parse failed: " << error;
  }
  return ok;
}

TEST(UnifiedDiffTest, ParsesAnAddedFile) {
  Patch patch;
  ASSERT_TRUE(
      Parse("diff --git a/solutions/x/strategy.h b/solutions/x/strategy.h\n"
            "new file mode 100644\n"
            "--- /dev/null\n"
            "+++ b/solutions/x/strategy.h\n"
            "@@ -0,0 +1,2 @@\n"
            "+#include <cstdio>\n"
            "+int f() { return 1; }\n",
            &patch));
  ASSERT_EQ(patch.files.size(), 1u);
  EXPECT_TRUE(patch.files[0].is_new);
  EXPECT_EQ(patch.files[0].path(), "solutions/x/strategy.h");
  EXPECT_EQ(patch.files[0].hunks, 1);
  // The content comes back so the leaderboard can show source without a
  // checkout.
  EXPECT_EQ(patch.files[0].added_content,
            "#include <cstdio>\nint f() { return 1; }\n");
}

TEST(UnifiedDiffTest, ParsesAModificationAcrossSeveralFiles) {
  Patch patch;
  ASSERT_TRUE(
      Parse("diff --git a/core/a.h b/core/a.h\n"
            "--- a/core/a.h\n"
            "+++ b/core/a.h\n"
            "@@ -1,3 +1,3 @@\n"
            " keep\n"
            "-old\n"
            "+new\n"
            "@@ -20,2 +20,3 @@\n"
            " ctx\n"
            "+added\n"
            "diff --git a/core/b.cc b/core/b.cc\n"
            "--- a/core/b.cc\n"
            "+++ b/core/b.cc\n"
            "@@ -5,1 +5,1 @@\n"
            "-x\n"
            "+y\n",
            &patch));
  ASSERT_EQ(patch.files.size(), 2u);
  EXPECT_EQ(patch.files[0].path(), "core/a.h");
  EXPECT_EQ(patch.files[0].hunks, 2);
  EXPECT_FALSE(patch.files[0].is_new);
  // Only added files carry content; a modification's '+' lines are not a file.
  EXPECT_TRUE(patch.files[0].added_content.empty());
  EXPECT_EQ(patch.files[1].path(), "core/b.cc");
  EXPECT_EQ(patch.total_hunks, 3);
}

TEST(UnifiedDiffTest, ParsesADeletion) {
  Patch patch;
  ASSERT_TRUE(
      Parse("diff --git a/core/gone.h b/core/gone.h\n"
            "deleted file mode 100644\n"
            "--- a/core/gone.h\n"
            "+++ /dev/null\n"
            "@@ -1,2 +0,0 @@\n"
            "-a\n"
            "-b\n",
            &patch));
  ASSERT_EQ(patch.files.size(), 1u);
  EXPECT_TRUE(patch.files[0].is_delete);
  EXPECT_EQ(patch.files[0].path(), "core/gone.h");
}

TEST(UnifiedDiffTest, ParsesAPlainDiffUWithNoGitHeader) {
  Patch patch;
  ASSERT_TRUE(
      Parse("--- a/core/a.h\t2026-01-01 00:00:00\n"
            "+++ b/core/a.h\t2026-01-02 00:00:00\n"
            "@@ -1 +1 @@\n"
            "-x\n"
            "+y\n",
            &patch));
  ASSERT_EQ(patch.files.size(), 1u);
  // The tab-separated timestamp is not part of the path.
  EXPECT_EQ(patch.files[0].path(), "core/a.h");
}

TEST(UnifiedDiffTest, RejectsPathsThatEscapeTheRepo) {
  Patch patch;
  std::string error;
  EXPECT_FALSE(
      ParseUnifiedDiff("diff --git a/../../etc/passwd b/../../etc/passwd\n"
                       "new file mode 100644\n"
                       "--- /dev/null\n"
                       "+++ b/../../etc/passwd\n"
                       "@@ -0,0 +1,1 @@\n"
                       "+pwned\n",
                       &patch, &error));
  EXPECT_NE(error.find(".."), std::string::npos) << error;

  EXPECT_FALSE(
      ParseUnifiedDiff("diff --git a/x b/x\n--- /dev/null\n+++ /etc/shadow\n@@ "
                       "-0,0 +1,1 @@\n+x\n",
                       &patch, &error));
  EXPECT_NE(error.find("relative"), std::string::npos) << error;
}

TEST(UnifiedDiffTest, RejectsAnEmptyPatch) {
  Patch patch;
  std::string error;
  EXPECT_FALSE(ParseUnifiedDiff("", &patch, &error));
  EXPECT_NE(error.find("empty"), std::string::npos) << error;
  EXPECT_FALSE(ParseUnifiedDiff("not a diff at all\n", &patch, &error));
}

TEST(UnifiedDiffTest, TouchedPathsDeduplicates) {
  Patch patch;
  ASSERT_TRUE(
      Parse("diff --git a/core/a.h b/core/a.h\n"
            "--- a/core/a.h\n"
            "+++ b/core/a.h\n"
            "@@ -1 +1 @@\n-x\n+y\n",
            &patch));
  EXPECT_EQ(TouchedPaths(patch), std::vector<std::string>{"core/a.h"});
}

TEST(UnifiedDiffTest, RoundTripsAnAddOnlyPatch) {
  const std::string diff = MakeAddOnlyPatch({
      {"solutions/x/strategy.h", "line one\nline two\n"},
      {"solutions/x/BUILD", "cc_library()\n"},
  });
  Patch patch;
  ASSERT_TRUE(Parse(diff, &patch));
  ASSERT_EQ(patch.files.size(), 2u);
  EXPECT_TRUE(patch.files[0].is_new);
  EXPECT_EQ(patch.files[0].path(), "solutions/x/strategy.h");
  EXPECT_EQ(patch.files[0].added_content, "line one\nline two\n");
  EXPECT_EQ(patch.files[1].added_content, "cc_library()\n");
}

TEST(UnifiedDiffTest, PreservesAMissingTrailingNewline) {
  const std::string diff = MakeAddOnlyPatch({{"a.txt", "no trailing newline"}});
  EXPECT_NE(diff.find("\\ No newline at end of file"), std::string::npos)
      << diff;
  Patch patch;
  ASSERT_TRUE(Parse(diff, &patch));
  EXPECT_EQ(patch.files[0].added_content, "no trailing newline");
}

// The synthesized patch is the whole point of the structured submit form, and
// its only real contract is that `git apply` takes it. Asserting on the string
// would only prove this file agrees with itself.
TEST(UnifiedDiffTest, SynthesizedPatchIsAcceptedByGitApply) {
  if (std::system("git --version > /dev/null 2>&1") != 0) {
    GTEST_SKIP() << "git is not available";
  }
  const std::filesystem::path repo =
      std::filesystem::temp_directory_path() / "unified_diff_git_test";
  std::error_code ec;
  std::filesystem::remove_all(repo, ec);
  std::filesystem::create_directories(repo, ec);
  ASSERT_FALSE(ec);
  ASSERT_EQ(
      std::system(
          ("git -C " + repo.string() + " init -q > /dev/null 2>&1").c_str()),
      0);

  const std::string diff = MakeAddOnlyPatch({
      {"solutions/x/strategy.h", "#pragma once\nint f() { return 1; }\n"},
      {"solutions/x/BUILD", "cc_library(name = \"s\")\n"},
      {"solutions/x/no_newline.txt", "trailing"},
  });
  const std::filesystem::path patch_path = repo / "candidate.diff";
  {
    std::ofstream out(patch_path);
    out << diff;
  }

  ASSERT_EQ(std::system(("git -C " + repo.string() + " apply --check " +
                         patch_path.string())
                            .c_str()),
            0)
      << "git apply --check rejected:\n"
      << diff;
  ASSERT_EQ(
      std::system(("git -C " + repo.string() + " apply " + patch_path.string())
                      .c_str()),
      0);

  // And it applied to the right paths with the right bytes.
  std::ifstream applied(repo / "solutions/x/strategy.h");
  const std::string content((std::istreambuf_iterator<char>(applied)),
                            std::istreambuf_iterator<char>());
  EXPECT_EQ(content, "#pragma once\nint f() { return 1; }\n");

  std::ifstream no_nl(repo / "solutions/x/no_newline.txt");
  const std::string tail((std::istreambuf_iterator<char>(no_nl)),
                         std::istreambuf_iterator<char>());
  EXPECT_EQ(tail, "trailing") << "a trailing newline was invented";

  std::filesystem::remove_all(repo, ec);
}

TEST(UnifiedDiffTest, GlobMatchesWithinAndAcrossSegments) {
  // '**' crosses separators.
  EXPECT_TRUE(
      PathMatchesGlob("problem/lib/core.h", "problem/lib/**"));
  EXPECT_TRUE(PathMatchesGlob("a/b/c/d.h", "a/**/d.h"));
  EXPECT_TRUE(PathMatchesGlob("a/d.h", "a/**/d.h")) << "** also matches none";
  EXPECT_FALSE(
      PathMatchesGlob("problem/tools/bench.cc", "problem/lib/**"));

  // A single '*' stops at a separator.
  EXPECT_TRUE(PathMatchesGlob("core/a.h", "core/*.h"));
  EXPECT_FALSE(PathMatchesGlob("core/sub/a.h", "core/*.h"));
  EXPECT_TRUE(PathMatchesGlob("core/sub/BUILD", "**/BUILD"));
  EXPECT_TRUE(PathMatchesGlob("BUILD", "**/BUILD")) << "top level counts too";

  EXPECT_TRUE(PathMatchesGlob("exact/path.h", "exact/path.h"));
  EXPECT_FALSE(PathMatchesGlob("exact/path.h", "exact/other.h"));
  EXPECT_FALSE(PathMatchesGlob("core/x_test.cc", "**/*_test.cpp"));
  EXPECT_TRUE(PathMatchesGlob("core/x_test.cc", "**/*_test.cc"));
}

}  // namespace
}  // namespace tournament_arena
