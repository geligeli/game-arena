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

TEST(VolumeMountTest, NamedVolumeWithOptionalReadonly) {
  EXPECT_EQ(VolumeMount("job-ws", "/workspace", false),
            "type=volume,source=job-ws,target=/workspace");
  EXPECT_EQ(VolumeMount("job-patches", "/patches", true),
            "type=volume,source=job-patches,target=/patches,readonly");
}

TEST(ScratchPreludeTest, OnlyExportsHome) {
  // The tree and scratch are mounted already; nothing to assemble, but bazel
  // insists on a writable HOME and the root filesystem is read-only.
  EXPECT_EQ(ScratchPrelude(), "export HOME=/sandbox\n");
}

TEST(DockerRunArgsTest, CreateMakesAContainerWithoutStartingIt) {
  DockerRunSpec spec;
  spec.name = "job-load";
  spec.image = "img:1";
  spec.script = "true\n";
  spec.create = true;
  spec.network = "none";
  spec.mounts = {"type=volume,source=job-ws,target=/workspace"};
  const std::vector<std::string> args = DockerRunArgs(spec);
  // No --rm and no -d: those are `run` options, and a created container is
  // filled by `docker cp` and started afterwards.
  const std::vector<std::string> expected = {
      "create",
      "--name",
      "job-load",
      "--network",
      "none",
      "--mount",
      "type=volume,source=job-ws,target=/workspace",
      "--entrypoint",
      "/bin/sh",
      "img:1",
      "-c",
      "true\n"};
  EXPECT_EQ(args, expected);
}

TEST(DockerRunArgsTest, FixedShapeWithAllOptions) {
  const std::vector<std::string> args = DockerRunArgs({
      /*name=*/"saw-0-o1-build",
      /*image=*/"img:1",
      /*script=*/"set -eu\n",
      /*rm=*/true,
      /*detached=*/false,
      /*create=*/false,
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
      /*create=*/false,
  });
  const std::vector<std::string> expected = {
      "run", "--name", "n", "-d", "--entrypoint", "/bin/sh", "img", "-c", "s",
  };
  EXPECT_EQ(args, expected);
}

}  // namespace
}  // namespace sandbox_common
