// Every problem config in problems/ must load.
//
// These files are the deployment surface: a typo in one is only discovered when
// a server refuses to start, which is the worst moment to find out. Parsing
// them here moves that to build time.

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

#include "game_mcts/tournament_server/server/problem_config.h"

namespace tournament_arena {
namespace {

// Runfiles-relative; a cc_test runs with the runfiles root as its cwd.
constexpr char kProblemsDir[] = "game_mcts/tournament_server/problems";

TEST(ShippedProblemsTest, EveryConfigLoads) {
  ASSERT_TRUE(std::filesystem::is_directory(kProblemsDir))
      << "problems/ not in runfiles; check the test's data dependency";

  std::vector<std::string> loaded;
  for (const auto &entry : std::filesystem::directory_iterator(kProblemsDir)) {
    if (entry.path().extension() != ".textproto") {
      continue;
    }
    std::string error;
    const auto config = LoadProblemConfig(entry.path(), &error);
    EXPECT_TRUE(config.has_value()) << error;
    if (config) {
      loaded.push_back(config->problem_id());
    }
  }

  // A glob that silently matches nothing would make this test pass forever.
  EXPECT_GE(loaded.size(), 2u)
      << "expected the shipped match and grade problems";
  EXPECT_NE(std::find(loaded.begin(), loaded.end(), "risk2"), loaded.end());
  EXPECT_NE(std::find(loaded.begin(), loaded.end(), "mcts-bench"),
            loaded.end());
}

}  // namespace
}  // namespace tournament_arena
