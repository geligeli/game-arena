load("@protobuf//bazel:cc_proto_library.bzl", "cc_proto_library")
load("@protobuf//bazel:proto_library.bzl", "proto_library")
load("@rules_cc//cc:cc_library.bzl", "cc_library")
load("@rules_cc//cc:cc_test.bzl", "cc_test")

package(default_visibility = ["//visibility:public"])

# A sandbox execution engine, and nothing about what the sandbox is for.
#
# The property worth checking when you add a dep here: no target in this
# package may depend on anything under //game_arena/proto, or on
# //game_arena/server, //game_arena/referee or //game_arena/sandbox/worker.
# The arena's own policy -- orders, matches, ELO, metrics -- lives above this,
# in //game_arena/sandbox/worker.
#
#   bazel query 'somepath(//game_arena/sandbox/exec/..., //game_arena/proto/...)'
#
# should stay empty.

proto_library(
    name = "sandbox_job_proto",
    srcs = ["sandbox_job.proto"],
)

cc_proto_library(
    name = "sandbox_job_cc_proto",
    deps = [":sandbox_job_proto"],
)

cc_library(
    name = "engine",
    srcs = ["engine.cc"],
    hdrs = ["engine.h"],
    deps = [
        ":sandbox_job_cc_proto",
        "//game_arena/sandbox/common:docker",
    ],
)

cc_library(
    name = "checkout",
    srcs = ["checkout.cc"],
    hdrs = ["checkout.h"],
    deps = [
        "//game_arena/sandbox/common:step",
        "//game_arena/sandbox/common:text",
    ],
)

cc_library(
    name = "entrypoint",
    srcs = ["entrypoint.cc"],
    hdrs = ["entrypoint.h"],
    deps = [
        ":sandbox_job_cc_proto",
        "//game_arena/sandbox/common:docker",
    ],
)

cc_test(
    name = "entrypoint_test",
    srcs = ["entrypoint_test.cc"],
    deps = [
        ":entrypoint",
        "@googletest//:gtest_main",
    ],
)

cc_library(
    name = "isolation",
    srcs = ["isolation.cc"],
    hdrs = ["isolation.h"],
    deps = [":sandbox_job_cc_proto"],
)

cc_test(
    name = "isolation_test",
    srcs = ["isolation_test.cc"],
    deps = [
        ":isolation",
        "@googletest//:gtest_main",
    ],
)

cc_library(
    name = "workspace",
    srcs = ["workspace.cc"],
    hdrs = ["workspace.h"],
    deps = [
        ":checkout",
        ":sandbox_job_cc_proto",
        "//game_arena/common/process",
        "//game_arena/sandbox/common:docker",
        "//game_arena/sandbox/common:files",
        "//game_arena/sandbox/common:step",
        "//game_arena/sandbox/common:text",
    ],
)

cc_test(
    name = "workspace_test",
    srcs = ["workspace_test.cc"],
    deps = [
        ":workspace",
        "@googletest//:gtest_main",
    ],
)

cc_library(
    name = "container_engine",
    srcs = ["container_engine.cc"],
    hdrs = ["container_engine.h"],
    deps = [
        ":engine",
        ":entrypoint",
        ":isolation",
        ":sandbox_job_cc_proto",
        ":workspace",
        "//game_arena/common/process",
        "//game_arena/sandbox/common:docker",
        "//game_arena/sandbox/common:files",
        "//game_arena/sandbox/common:step",
        "//game_arena/sandbox/common:text",
    ],
)

cc_test(
    name = "container_engine_test",
    srcs = ["container_engine_test.cc"],
    deps = [
        ":container_engine",
        "@googletest//:gtest_main",
    ],
)

cc_library(
    name = "process_engine",
    srcs = ["process_engine.cc"],
    hdrs = ["process_engine.h"],
    deps = [
        ":engine",
        ":sandbox_job_cc_proto",
        ":workspace",
        "//game_arena/common/process",
        "//game_arena/sandbox/common:files",
        "//game_arena/sandbox/common:step",
        "//game_arena/sandbox/common:text",
    ],
)

cc_test(
    name = "process_engine_test",
    srcs = ["process_engine_test.cc"],
    deps = [
        ":process_engine",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "engine_test",
    srcs = ["engine_test.cc"],
    deps = [
        ":engine",
        "@googletest//:gtest_main",
    ],
)
