#include "game_arena/server/artifact_store.h"

#include <cstdlib>
#include <filesystem>
#include <string>

#include "game_arena/common/sha256/sha256.h"
#include "gtest/gtest.h"

namespace tournament_arena {
namespace {

std::filesystem::path Dir() {
  return std::filesystem::path(std::getenv("TEST_TMPDIR")) /
         ::testing::UnitTest::GetInstance()->current_test_info()->name();
}

TEST(ArtifactStoreTest, WhatIsPutComesBackByItsDigest) {
  ArtifactStore store(Dir());
  const std::string bot =
      "\x7f"
      "ELF a bot";
  const std::string digest = sha256::Hex(bot);
  std::string error;
  EXPECT_FALSE(store.Has(digest));
  ASSERT_TRUE(store.Put(digest, bot, &error)) << error;
  EXPECT_TRUE(store.Has(digest));
  EXPECT_EQ(store.Get(digest), bot);
  // Again, from disk.
  EXPECT_EQ(ArtifactStore(Dir()).Get(digest), bot);
}

TEST(ArtifactStoreTest, BytesThatAreNotTheirDigestAreRefused) {
  ArtifactStore store(Dir());
  std::string error;
  EXPECT_FALSE(store.Put(sha256::Hex("a bot"), "another bot", &error));
  EXPECT_FALSE(store.Put("../../etc/passwd", "x", &error));
  EXPECT_FALSE(store.Get("../../etc/passwd").has_value());
}

TEST(ArtifactStoreTest, ARefNamesAStoredDigestAcrossRestarts) {
  ArtifactStore store(Dir());
  const std::string digest = sha256::Hex("a referee");
  EXPECT_FALSE(store.SetRef("referee-1", digest));  // not stored yet
  std::string error;
  ASSERT_TRUE(store.Put(digest, "a referee", &error));
  ASSERT_TRUE(store.SetRef("referee-1", digest));
  EXPECT_EQ(ArtifactStore(Dir()).Ref("referee-1"), digest);
  EXPECT_EQ(store.Ref("referee-2"), "");
  EXPECT_FALSE(store.SetRef("../escape", digest));
}

}  // namespace
}  // namespace tournament_arena
