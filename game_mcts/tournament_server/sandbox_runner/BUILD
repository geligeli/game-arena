load("@rules_cc//cc:cc_binary.bzl", "cc_binary")
load("@rules_cc//cc:cc_library.bzl", "cc_library")
load("@rules_cc//cc:cc_test.bzl", "cc_test")

package(default_visibility = ["//visibility:public"])

# Standalone sandbox runner: serves SandboxService (sandbox_runner.proto) by
# running `bazel run //<target>` inside a throwaway docker container per
# request. Patches are staged on the host, mounted in, and copied onto an
# overlay whose lower dir is --repo_dir, so the host checkout stays untouched.

cc_library(
    name = "sandbox_runner_lib",
    srcs = ["sandbox_runner.cc"],
    hdrs = ["sandbox_runner.h"],
    deps = [
        "//game_mcts/cpp/process",
        "//game_mcts/tournament_server:sandbox_runner_cc_grpc",
        "@abseil-cpp//absl/log",
        "@grpc//:grpc++",
    ],
)

cc_binary(
    name = "sandbox_runner",
    srcs = ["main.cc"],
    deps = [
        ":sandbox_runner_lib",
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
        "//game_mcts/tournament_server:sandbox_runner_cc_grpc",
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
        "//game_mcts/tournament_server:sandbox_runner_cc_grpc",
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
