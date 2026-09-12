// One builder against the four scripts it replaces.
//
// The expectations here are lifted from the hand-assembled versions in
// sandbox/worker (build, run, grade) and sandbox/runner (copy-and-run), so
// this file is the evidence that collapsing them changed no emitted byte.

#include "game_arena/sandbox/exec/entrypoint.h"

#include <string>

#include "gtest/gtest.h"

namespace sandbox_exec {
namespace {

auto Word(const std::string &text, bool verbatim) -> proto::Token {
  proto::Token token;
  token.set_text(text);
  token.set_verbatim(verbatim);
  return token;
}

// A workspace whose overlay the entrypoint assembles itself. The host-mounted
// form leaves nothing for the script to do, so this is the one with a script
// worth asserting on.
auto InSandboxOverlay() -> proto::Workspace {
  proto::Workspace ws;
  ws.set_overlay(proto::Workspace::OVERLAY_IN_SANDBOX);
  ws.set_patch(proto::Workspace::PATCH_IN_ENTRYPOINT);
  return ws;
}

TEST(EntrypointScriptTest, ReproducesTheBuildScript) {
  proto::Workspace ws = InSandboxOverlay();
  ws.add_patch_files("c-1.diff");

  proto::Step step;
  step.set_applies_patches(true);
  *step.add_argv() = Word("bazel", true);
  *step.add_argv() = Word("--output_base=/output_base", true);
  *step.add_argv() = Word("build", true);
  *step.add_argv() = Word("--disk_cache=/disk_cache", true);
  *step.add_argv() = Word("--config=native", false);
  *step.add_argv() = Word("//problem/x:c-1:bot", false);

  const std::string script = EntrypointScript(ws, step);

  EXPECT_NE(script.find("set -eu"), std::string::npos);
  EXPECT_NE(script.find("mkdir -p /sandbox/upper /sandbox/work /workspace"),
            std::string::npos);
  EXPECT_NE(script.find("mount -t overlay overlay -o lowerdir=/repo_lower,"
                        "upperdir=/sandbox/upper,workdir=/sandbox/work "
                        "/workspace"),
            std::string::npos);
  EXPECT_NE(script.find("git apply '/patches/c-1.diff'"), std::string::npos)
      << script;
  // The interleaving of quoted and unquoted words is why a token carries its
  // own quoting rather than the step having a command half and an args half.
  // --output_base is a startup option, so it precedes the command;
  // --disk_cache and the problem's flags are command options and follow it.
  EXPECT_NE(script.find("exec bazel --output_base=/output_base build "
                        "--disk_cache=/disk_cache '--config=native' "
                        "'//problem/x:c-1:bot'"),
            std::string::npos)
      << script;
}

TEST(EntrypointScriptTest, AppliesEverySidesPatchBeforeTheCommand) {
  proto::Workspace ws = InSandboxOverlay();
  ws.add_patch_files("alpha.diff");
  ws.add_patch_files("beta.diff");

  proto::Step step;
  step.set_applies_patches(true);
  *step.add_argv() = Word("bazel", true);
  *step.add_argv() = Word("build", true);
  *step.add_argv() = Word("//a:bot", false);

  const std::string script = EntrypointScript(ws, step);
  const auto alpha = script.find("git apply '/patches/alpha.diff'");
  const auto beta = script.find("git apply '/patches/beta.diff'");
  const auto build = script.find("build '//a:bot'");
  ASSERT_NE(alpha, std::string::npos) << script;
  ASSERT_NE(beta, std::string::npos) << script;
  // Both sides land before the build runs: a match builds one tree, not two.
  EXPECT_LT(alpha, build);
  EXPECT_LT(beta, build);
  EXPECT_LT(alpha, beta);
}

TEST(EntrypointScriptTest, AStepThatDoesNotPatchInheritsTheTree) {
  // The graded run's container: the build container already applied the
  // patch, and it persists in the overlay upper.
  proto::Workspace ws = InSandboxOverlay();
  ws.add_patch_files("c-1.diff");

  proto::Step step;
  step.set_applies_patches(false);
  *step.add_argv() = Word("./bench", false);

  const std::string script = EntrypointScript(ws, step);
  EXPECT_EQ(script.find("git apply"), std::string::npos) << script;
  EXPECT_NE(script.find("exec './bench'"), std::string::npos);
}

TEST(EntrypointScriptTest, ReproducesTheRunScript) {
  proto::Workspace ws;
  ws.set_overlay(proto::Workspace::OVERLAY_IN_SANDBOX);

  proto::Step step;
  *step.add_argv() = Word("./bazel-bin/problem/x/c-1/bot", false);
  *step.add_argv() = Word("--name=c-1", false);
  *step.add_argv() = Word("--opponent=builtin:random", false);

  EXPECT_NE(EntrypointScript(ws, step).find(
                "exec './bazel-bin/problem/x/c-1/bot' '--name=c-1' "
                "'--opponent=builtin:random'"),
            std::string::npos);
}

TEST(EntrypointScriptTest, ReproducesTheGradeScriptsExportedReportPath) {
  proto::Workspace ws = InSandboxOverlay();

  proto::Step step;
  (*step.mutable_env())["ARENA_REPORT"] = "/sandbox/report.json";
  *step.add_argv() = Word("./bench", false);
  *step.add_argv() = Word("--n=10", false);

  const std::string script = EntrypointScript(ws, step);
  // Exported rather than fixed, so the command needs no knowledge of the
  // sandbox's directory layout.
  EXPECT_NE(script.find("export ARENA_REPORT='/sandbox/report.json'"),
            std::string::npos)
      << script;
  EXPECT_LT(script.find("export ARENA_REPORT="), script.find("exec './bench'"));
}

TEST(EntrypointScriptTest, ReproducesTheStandaloneRunnersCopyAndRun) {
  proto::Workspace ws;
  ws.set_overlay(proto::Workspace::OVERLAY_IN_SANDBOX);
  ws.set_patch(proto::Workspace::PATCH_COPY_IN_ENTRYPOINT);

  proto::Step step;
  step.set_applies_patches(true);
  *step.add_argv() = Word("bazel", true);
  *step.add_argv() = Word("run", true);
  *step.add_argv() = Word("//problem/app:target", false);
  *step.add_argv() = Word("--", true);
  *step.add_argv() = Word("--flag=1", false);
  *step.add_argv() = Word("a b", false);

  const std::string script = EntrypointScript(ws, step);
  EXPECT_NE(script.find("cp -a /patches/. /workspace/"), std::string::npos)
      << script;
  EXPECT_NE(script.find("exec bazel run '//problem/app:target' -- "
                        "'--flag=1' 'a b'"),
            std::string::npos)
      << script;
}

TEST(EntrypointScriptTest, AHostMountedOverlayLeavesNothingToAssemble) {
  proto::Workspace ws;
  ws.set_overlay(proto::Workspace::OVERLAY_HOST);

  proto::Step step;
  *step.add_argv() = Word("./bot", false);

  const std::string script = EntrypointScript(ws, step);
  EXPECT_EQ(script.find("mount"), std::string::npos) << script;
  // Still a writable HOME, which bazel insists on.
  EXPECT_NE(script.find("export HOME=/sandbox"), std::string::npos);
}

TEST(EntrypointScriptTest, EnvIsSortedSoTwoEqualStepsAreByteIdentical) {
  proto::Workspace ws;
  proto::Step step;
  (*step.mutable_env())["ZULU"] = "1";
  (*step.mutable_env())["ALPHA"] = "2";
  *step.add_argv() = Word("./x", false);

  const std::string script = EntrypointScript(ws, step);
  EXPECT_LT(script.find("export ALPHA="), script.find("export ZULU="));
}

TEST(EntrypointScriptTest, StepCwdOverridesTheWorkspaceRoot) {
  proto::Workspace ws;
  ws.set_sandbox_work_dir("/workspace");
  proto::Step step;
  step.set_cwd("/workspace/sub");
  *step.add_argv() = Word("./x", false);

  EXPECT_NE(EntrypointScript(ws, step).find("cd /workspace/sub\n"),
            std::string::npos);
}

}  // namespace
}  // namespace sandbox_exec
