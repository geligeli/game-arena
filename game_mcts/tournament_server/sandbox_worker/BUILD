load("@rules_cc//cc:cc_binary.bzl", "cc_binary")
load("@rules_cc//cc:cc_library.bzl", "cc_library")
load("@rules_cc//cc:cc_test.bzl", "cc_test")

package(default_visibility = ["//visibility:public"])

# The sandbox fleet: hosts that check out the repo, patch a submitted candidate
# in, build it and play its games. Workers dial the arena, so a fleet can be
# attached from anywhere.

# The BUILD file a submission is compiled with. Pure string generation, so the
# exact bazel a candidate produces can be asserted directly.
cc_library(
    name = "candidate_build",
    srcs = ["candidate_build.cc"],
    hdrs = ["candidate_build.h"],
    deps = ["//game_mcts/tournament_server:arena_cc_proto"],
)

# Compacts bazel output before it crosses the wire: the system's biggest token
# lever.
cc_library(
    name = "build_log",
    srcs = ["build_log.cc"],
    hdrs = ["build_log.h"],
    deps = ["@re2"],
)

cc_library(
    name = "sandbox_backend",
    hdrs = ["sandbox_backend.h"],
    deps = ["//game_mcts/tournament_server:arena_cc_proto"],
)

cc_library(
    name = "local_backend",
    srcs = ["local_backend.cc"],
    hdrs = ["local_backend.h"],
    deps = [
        ":build_log",
        ":candidate_build",
        ":sandbox_backend",
        "//game_mcts/cpp/process",
        "@abseil-cpp//absl/log",
    ],
)

# The docker backend: same order steps as local_backend, but each phase runs in
# a throwaway container over a per-slot overlay (host does the git work).
cc_library(
    name = "docker_backend",
    srcs = ["docker_backend.cc"],
    hdrs = ["docker_backend.h"],
    deps = [
        ":build_log",
        ":candidate_build",
        ":sandbox_backend",
        "//game_mcts/cpp/process",
        "@abseil-cpp//absl/log",
    ],
)

cc_binary(
    name = "sandbox_worker",
    srcs = ["worker_main.cc"],
    deps = [
        ":docker_backend",
        ":local_backend",
        ":sandbox_backend",
        "//game_mcts/tournament_server:arena_cc_grpc",
        "@abseil-cpp//absl/flags:flag",
        "@abseil-cpp//absl/flags:parse",
        "@abseil-cpp//absl/log",
        "@abseil-cpp//absl/log:globals",
        "@abseil-cpp//absl/log:initialize",
        "@abseil-cpp//absl/strings",
        "@grpc//:grpc++",
    ],
)

cc_test(
    name = "candidate_build_test",
    srcs = ["candidate_build_test.cc"],
    deps = [
        ":candidate_build",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "build_log_test",
    srcs = ["build_log_test.cc"],
    deps = [
        ":build_log",
        "@googletest//:gtest_main",
    ],
)

# The pure helpers: quoting, name sanitisation and the exact entrypoint script
# handed to /bin/sh, asserted without a docker daemon.
cc_test(
    name = "docker_backend_test",
    srcs = ["docker_backend_test.cc"],
    deps = [
        ":docker_backend",
        "@googletest//:gtest_main",
    ],
)

# End-to-end against fake `docker` and `git` scripts -- no daemon needed.
# Covers the mounts, the entrypoint scripts, host-side checkout, build failure
# reporting, run timeouts and patch-path validation.
cc_test(
    name = "docker_backend_integration_test",
    srcs = ["docker_backend_integration_test.cc"],
    deps = [
        ":docker_backend",
        "@googletest//:gtest_main",
    ],
)
