load("@rules_cc//cc:cc_library.bzl", "cc_library")
load("@rules_cc//cc:cc_test.bzl", "cc_test")

package(default_visibility = ["//visibility:public"])

# Mechanics shared by the standalone sandbox runner (sandbox/runner) and the
# fleet worker's backends (sandbox/worker), so the two cannot drift into
# spelling the same step, mount or container differently: a patch staged for
# one means the same thing to the other.

cc_library(
    name = "files",
    srcs = ["files.cc"],
    hdrs = ["files.h"],
)

cc_test(
    name = "files_test",
    srcs = ["files_test.cc"],
    deps = [
        ":files",
        "@googletest//:gtest_main",
    ],
)

# Trimming captured output. Every layer reports a failure; only one of them
# knows what bazel is, so this is here rather than beside the build-log
# compactor it used to share a target with.
cc_library(
    name = "text",
    srcs = ["text.cc"],
    hdrs = ["text.h"],
)

cc_test(
    name = "text_test",
    srcs = ["text_test.cc"],
    deps = [
        ":text",
        "@googletest//:gtest_main",
    ],
)

# One subprocess step with its output captured to files, the same shape
# whether the step runs on the host or starts a container.
cc_library(
    name = "step",
    srcs = ["step.cc"],
    hdrs = ["step.h"],
    deps = [
        ":files",
        "//game_arena/common/process",
    ],
)

# Docker mechanics: container naming, bind-mount syntax, the fixed
# in-container mount points, entrypoint script preludes, lifecycle helpers,
# and the one shape a `docker run` argv takes.
cc_library(
    name = "docker",
    srcs = ["docker.cc"],
    hdrs = ["docker.h"],
    deps = ["//game_arena/common/process"],
)

cc_test(
    name = "docker_test",
    srcs = ["docker_test.cc"],
    deps = [
        ":docker",
        "@googletest//:gtest_main",
    ],
)
