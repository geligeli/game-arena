#include "game_arena/common/kv_options/kv_options.h"

#include <map>
#include <string>

#include "gtest/gtest.h"

namespace kv_options {
namespace {

using Options = std::map<std::string, std::string>;

TEST(Parse, ReadsPairs) {
  EXPECT_EQ(Parse("a=1,b=2"), (Options{{"a", "1"}, {"b", "2"}}));
  EXPECT_EQ(Parse("only=one"), (Options{{"only", "one"}}));
  EXPECT_TRUE(Parse("").empty());
}

TEST(Parse, KeepsValuesThatLookAwkward) {
  EXPECT_EQ(Parse("empty="), (Options{{"empty", ""}}));
  EXPECT_EQ(Parse("path=/a/b/c"), (Options{{"path", "/a/b/c"}}));
  EXPECT_EQ(Parse("spaced=a b"), (Options{{"spaced", "a b"}}));
}

// These bytes are built by another process. A referee that refused to start
// over one malformed entry would fail an order it could have run.
TEST(Parse, SkipsMalformedEntriesRatherThanFailing) {
  EXPECT_EQ(Parse("a=1,,b=2"), (Options{{"a", "1"}, {"b", "2"}}));
  EXPECT_EQ(Parse("a=1,noequals,b=2"), (Options{{"a", "1"}, {"b", "2"}}));
  EXPECT_EQ(Parse("=novalue,a=1"), (Options{{"a", "1"}}));
  EXPECT_EQ(Parse(",,,"), Options{});
}

TEST(Parse, LastDuplicateWins) {
  EXPECT_EQ(Parse("a=1,a=2"), (Options{{"a", "2"}}));
}

TEST(Format, IsStableAndSorted) {
  EXPECT_EQ(Format({{"b", "2"}, {"a", "1"}}), "a=1,b=2");
  EXPECT_EQ(Format({}), "");
}

TEST(Format, DropsEntriesThatWouldNotSurviveTheRoundTrip) {
  EXPECT_EQ(Format({{"ok", "1"}, {"bad,key", "2"}}), "ok=1");
  EXPECT_EQ(Format({{"ok", "1"}, {"bad", "has=eq"}}), "ok=1");
  EXPECT_EQ(Format({{"", "1"}, {"ok", "2"}}), "ok=2");
}

TEST(RoundTrip, SurvivesFormatThenParse) {
  const Options options{{"mcts_iterations", "400"}, {"depth", "7"}};
  EXPECT_EQ(Parse(Format(options)), options);
}

TEST(Validity, RejectsSeparatorsOnly) {
  EXPECT_TRUE(IsValidKey("mcts_iterations"));
  EXPECT_FALSE(IsValidKey(""));
  EXPECT_FALSE(IsValidKey("a,b"));
  EXPECT_FALSE(IsValidKey("a=b"));
  EXPECT_TRUE(IsValidValue(""));
  EXPECT_TRUE(IsValidValue("400"));
  EXPECT_FALSE(IsValidValue("a,b"));
}

}  // namespace
}  // namespace kv_options
