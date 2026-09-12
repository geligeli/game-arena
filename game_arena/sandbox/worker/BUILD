load("@rules_cc//cc:cc_binary.bzl", "cc_binary")
load("@rules_cc//cc:cc_library.bzl", "cc_library")
load("@rules_cc//cc:cc_test.bzl", "cc_test")

package(default_visibility = ["//visibility:public"])

# The sandbox fleet: hosts that check out the repo, patch a submitted candidate
# in, build it and play its games. Workers dial the arena, so a fleet can be
# attached from anywhere.

# Turning an order's bazel labels and params into something to exec. Shared by
# both backends so they cannot drift into launching the same order differently.
cc_library(
    name = "bot_launch",
    srcs = ["bot_launch.cc"],
    hdrs = ["bot_launch.h"],
    deps = ["@protobuf//:protobuf"],
)

# Reading what a graded command measured, and folding several runs into one
# number per metric.
cc_library(
    name = "metric_report",
    srcs = ["metric_report.cc"],
    hdrs = ["metric_report.h"],
    deps = [
        "//game_arena/proto:arena_cc_proto",
        "//game_arena/proto:problem_cc_proto",
        "@protobuf//:protobuf",
    ],
)

cc_test(
    name = "metric_report_test",
    srcs = ["metric_report_test.cc"],
    deps = [
        ":metric_report",
        "@googletest//:gtest_main",
    ],
)

# Compacts bazel output before it crosses the wire: the system's biggest token
# lever.
cc_library(
    name = "build_log",
    srcs = ["build_log.cc"],
    hdrs = ["build_log.h"],
    deps = [
        "//game_arena/sandbox/common:text",
        "@re2",
    ],
)

# The RESULT line a refereed match prints. Arena vocabulary -- games, draws,
# an ELO number -- kept out of build_log, which is generic build tooling.
cc_library(
    name = "match_tally",
    srcs = ["match_tally.cc"],
    hdrs = ["match_tally.h"],
    deps = ["@re2"],
)

cc_test(
    name = "match_tally_test",
    srcs = ["match_tally_test.cc"],
    deps = [
        ":match_tally",
        "@googletest//:gtest_main",
    ],
)

# The host-side git work both backends do per slot: clone once, then fetch
# and force-checkout the order's base commit. Shared so the two backends
# cannot drift into running the same order on different trees.
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
    name = "sandbox_backend",
    hdrs = ["sandbox_backend.h"],
    deps = ["//game_arena/proto:arena_cc_proto"],
)

cc_library(
    name = "local_backend",
    srcs = ["local_backend.cc"],
    hdrs = ["local_backend.h"],
    deps = [
        "//game_arena/common/kv_options",
        ":build_log",
        ":checkout",
        ":match_tally",
        ":metric_report",
        ":bot_launch",
        ":sandbox_backend",
        "//game_arena/common/process",
        "//game_arena/sandbox/common:files",
        "//game_arena/sandbox/common:text",
        "//game_arena/sandbox/common:step",
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
        "//game_arena/common/kv_options",
        ":build_log",
        ":checkout",
        ":match_tally",
        ":metric_report",
        ":bot_launch",
        ":sandbox_backend",
        "//game_arena/common/process",
        "//game_arena/sandbox/common:docker",
        "//game_arena/sandbox/common:files",
        "//game_arena/sandbox/common:text",
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
        "//game_arena/proto:arena_cc_grpc",
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
    name = "build_log_test",
    srcs = ["build_log_test.cc"],
    deps = [
        ":build_log",
        "@googletest//:gtest_main",
    ],
)

# The order-specific entrypoint scripts, asserted without a docker daemon.
# Quoting, name sanitisation and the overlay preludes are tested in
# sandbox/common:docker_test.
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

# The graded path end to end: fake git and bazel, but a real command writing a
# real report through $ARENA_REPORT.
cc_test(
    name = "local_backend_integration_test",
    srcs = ["local_backend_integration_test.cc"],
    deps = [
        ":local_backend",
        "@googletest//:gtest_main",
    ],
)
