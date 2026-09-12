// TailOf: keep the end, say what was dropped.

#include "game_arena/sandbox/common/text.h"

#include <string>

#include "gtest/gtest.h"

namespace sandbox_common {
namespace {

TEST(TailOfTest, MarksWhatItDropped) {
  EXPECT_EQ(TailOf("short", 100), "short");
  const std::string tail = TailOf(std::string(1000, 'x'), 100);
  EXPECT_NE(tail.find("900 chars truncated"), std::string::npos);
  EXPECT_LT(tail.size(), 200u);
}

}  // namespace
}  // namespace sandbox_common
