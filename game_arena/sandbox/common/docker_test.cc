// The pure docker helpers: shell quoting, container-name sanitisation, bind
// mount syntax, the entrypoint script preludes, and the one argv shape every
// `docker run` takes. Asserted directly, without a docker daemon.

#include "game_arena/sandbox/common/docker.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace sandbox_common {
namespace {

TEST(ShellQuoteTest, QuotesEverything) {
  EXPECT_EQ(ShellQuote("plain"), "'plain'");
  EXPECT_EQ(ShellQuote("a b"), "'a b'");
  EXPECT_EQ(ShellQuote("it's"), "'it'\\''s'");
  EXPECT_EQ(ShellQuote(""), "''");
  EXPECT_EQ(ShellQuote("$HOME `id` \"x\""), "'$HOME `id` \"x\"'");
  // A quote-injection attempt must stay one literal argument.
  EXPECT_EQ(ShellQuote("x'; rm -rf /; '"), "'x'\\''; rm -rf /; '\\'''");
}

TEST(SanitizeContainerNameTest, ReplacesInvalidCharacters) {
  // Docker names: [a-zA-Z0-9][a-zA-Z0-9_.-]*
  EXPECT_EQ(SanitizeContainerName("order-1.a_b"), "order-1.a_b");
  EXPECT_EQ(SanitizeContainerName("order/1: x"), "order-1--x");
  EXPECT_EQ(SanitizeContainerName("a b"), "a-b");
}

TEST(BindMountTest, LongFormWithOptionalReadonly) {
  EXPECT_EQ(BindMount("/host/dir", "/workspace", false),
            "type=bind,source=/host/dir,target=/workspace");
  EXPECT_EQ(BindMount("/host/dir", "/patches", true),
            "type=bind,source=/host/dir,target=/patches,readonly");
}

TEST(OverlayMountScriptTest, AssemblesUpperWorkAndMergedUnderOneScratch) {
  const std::string script = OverlayMountScript();
  // Upper and work under the single scratch mount: they must share a
  // filesystem, which overlayfs requires.
  EXPECT_NE(script.find("mkdir -p /sandbox/upper /sandbox/work /workspace"),
            std::string::npos);
  EXPECT_NE(script.find("export HOME=/sandbox"), std::string::npos);
  EXPECT_NE(script.find("mount -t overlay overlay -o lowerdir=/repo_lower,"
                        "upperdir=/sandbox/upper,workdir=/sandbox/work "
                        "/workspace"),
            std::string::npos);
}

TEST(ScratchSetupScriptTest, SetsUpScratchWithoutMounting) {
  const std::string script = ScratchSetupScript();
  EXPECT_NE(script.find("mkdir -p /sandbox/upper /sandbox/work /workspace"),
            std::string::npos);
  EXPECT_NE(script.find("export HOME=/sandbox"), std::string::npos);
  EXPECT_EQ(script.find("mount -t overlay"), std::string::npos);
}

TEST(HostOverlayPreludeTest, OnlyExportsHome) {
  // The host already mounted the merged tree; nothing to assemble, but bazel
  // insists on a writable HOME and the root filesystem is read-only.
  EXPECT_EQ(HostOverlayPrelude(), "export HOME=/sandbox\n");
}

TEST(DockerRunArgsTest, FixedShapeWithAllOptions) {
  const std::vector<std::string> args = DockerRunArgs({
      /*name=*/"saw-0-o1-build",
      /*image=*/"img:1",
      /*script=*/"set -eu\n",
      /*rm=*/true,
      /*detached=*/false,
      /*network=*/"none",
      /*extra_args=*/{"--cap-drop", "ALL"},
      /*mounts=*/{"type=bind,source=/h,target=/w"},
  });
  const std::vector<std::string> expected = {
      "run",          "--rm",
      "--name",       "saw-0-o1-build",
      "--cap-drop",   "ALL",
      "--network",    "none",
      "--mount",      "type=bind,source=/h,target=/w",
      "--entrypoint", "/bin/sh",
      "img:1",        "-c",
      "set -eu\n",
  };
  EXPECT_EQ(args, expected);
}

TEST(DockerRunArgsTest, OmitsRmNetworkAndMountsWhenUnset) {
  const std::vector<std::string> args = DockerRunArgs({
      /*name=*/"n",
      /*image=*/"img",
      /*script=*/"s",
      /*rm=*/false,
      /*detached=*/true,
  });
  const std::vector<std::string> expected = {
      "run", "--name", "n", "-d", "--entrypoint", "/bin/sh", "img", "-c", "s",
  };
  EXPECT_EQ(args, expected);
}

}  // namespace
}  // namespace sandbox_common
