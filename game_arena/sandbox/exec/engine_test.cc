// The one naming rule both engines and Cancel have to agree on.

#include "game_arena/sandbox/exec/engine.h"

#include <string>

#include "gtest/gtest.h"

namespace sandbox_exec {
namespace {

TEST(SandboxNameTest, AOneStepJobIsNamedAfterItself) {
  EXPECT_EQ(SandboxName("sbr-basic-1", ""), "sbr-basic-1");
}

TEST(SandboxNameTest, AStepSuffixesTheJob) {
  EXPECT_EQ(SandboxName("saw-0-ok-1", "build"), "saw-0-ok-1-build");
  EXPECT_EQ(SandboxName("saw-0-ok-1", "referee"), "saw-0-ok-1-referee");
}

TEST(SandboxNameTest, SanitisesBothHalves) {
  // Whatever a caller uses for an id, the sandbox name stays in docker's
  // alphabet.
  EXPECT_EQ(SandboxName("job/with:punct", "a step"), "job-with-punct-a-step");
}

}  // namespace
}  // namespace sandbox_exec
