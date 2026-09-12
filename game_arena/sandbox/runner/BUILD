load("@grpc//bazel:cc_grpc_library.bzl", "cc_grpc_library")
load("@protobuf//bazel:cc_proto_library.bzl", "cc_proto_library")
load("@protobuf//bazel:proto_library.bzl", "proto_library")
load("@rules_cc//cc:cc_binary.bzl", "cc_binary")
load("@rules_cc//cc:cc_library.bzl", "cc_library")
load("@rules_cc//cc:cc_test.bzl", "cc_test")

package(default_visibility = ["//visibility:public"])

# Standalone sandbox runner: serves SandboxService (sandbox_service.proto) by
# running a command in a throwaway sandbox per request, on the same engine the
# fleet worker uses (//game_arena/sandbox/exec). Files are staged on the host,
# mounted in, and copied onto an overlay whose lower dir is --repo_dir, so the
# host checkout stays untouched.

proto_library(
    name = "sandbox_service_proto",
    srcs = ["sandbox_service.proto"],
    deps = ["//game_arena/sandbox/exec:sandbox_job_proto"],
)

cc_proto_library(
    name = "sandbox_service_cc_proto",
    deps = [":sandbox_service_proto"],
)

cc_grpc_library(
    name = "sandbox_service_cc_grpc",
    srcs = [":sandbox_service_proto"],
    grpc_only = True,
    deps = [":sandbox_service_cc_proto"],
)

cc_library(
    name = "sandbox_runner_lib",
    srcs = ["sandbox_runner.cc"],
    hdrs = ["sandbox_runner.h"],
    deps = [
        "//game_arena/common/process",
        ":sandbox_service_cc_grpc",
        "//game_arena/sandbox/common:docker",
        "//game_arena/sandbox/common:files",
        "//game_arena/sandbox/exec:engine",
        "//game_arena/sandbox/exec:sandbox_job_cc_proto",
        "@abseil-cpp//absl/log",
        "@grpc//:grpc++",
    ],
)

cc_binary(
    name = "sandbox_runner",
    srcs = ["main.cc"],
    deps = [
        ":sandbox_runner_lib",
        "//game_arena/sandbox/exec:container_engine",
        "@abseil-cpp//absl/flags:flag",
        "@abseil-cpp//absl/flags:parse",
        "@abseil-cpp//absl/log",
        "@abseil-cpp//absl/log:globals",
        "@abseil-cpp//absl/log:initialize",
        "@grpc//:grpc++",
    ],
)

# Hand-driven client for a running sandbox_runner: one Run (or Kill) per
# invocation, with the target's output replayed locally. Testing only -- the
# tournament server talks to the same service programmatically.
cc_binary(
    name = "sandbox_cli",
    srcs = ["sandbox_cli.cc"],
    deps = [
        ":sandbox_service_cc_grpc",
        "@abseil-cpp//absl/flags:flag",
        "@abseil-cpp//absl/flags:parse",
        "@abseil-cpp//absl/log",
        "@abseil-cpp//absl/log:globals",
        "@abseil-cpp//absl/log:initialize",
        "@grpc//:grpc++",
    ],
)

# End-to-end against a fake `docker` script -- no daemon needed. Covers the
# docker argv, the entrypoint script, exit-code propagation, timeout kills and
# the Kill RPC.
cc_test(
    name = "sandbox_runner_integration_test",
    srcs = ["sandbox_runner_integration_test.cc"],
    deps = [
        ":sandbox_runner_lib",
        "//game_arena/sandbox/exec:container_engine",
        ":sandbox_service_cc_grpc",
        "@googletest//:gtest_main",
        "@grpc//:grpc++",
    ],
)

# Pure helpers only: anything past them needs a docker daemon.
cc_test(
    name = "sandbox_runner_test",
    srcs = ["sandbox_runner_test.cc"],
    deps = [
        ":sandbox_runner_lib",
        "@googletest//:gtest_main",
    ],
)
