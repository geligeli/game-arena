#include "game_arena/common/sha256/sha256.h"

#include "gtest/gtest.h"

namespace {

TEST(Sha256Test, KnownDigests) {
  EXPECT_EQ(sha256::Hex(""),
            "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  EXPECT_EQ(sha256::Hex("abc"),
            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

}  // namespace
