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
    name = "grade_policy",
    srcs = ["grade_policy.cc"],
    hdrs = ["grade_policy.h"],
    deps = ["//game_arena/proto:arena_cc_proto"],
)

cc_test(
    name = "grade_policy_test",
    srcs = ["grade_policy_test.cc"],
    deps = [
        ":grade_policy",
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

# The arena's own layer: what an order means, written once for both engines.
cc_library(
    name = "order_job",
    srcs = ["order_job.cc"],
    hdrs = ["order_job.h"],
    deps = [
        ":bot_launch",
        "//game_arena/common/kv_options",
        "//game_arena/proto:arena_cc_proto",
        "//game_arena/sandbox/common:docker",
        "//game_arena/sandbox/exec:engine",
        "//game_arena/sandbox/exec:sandbox_job_cc_proto",
    ],
)

cc_test(
    name = "order_job_test",
    srcs = ["order_job_test.cc"],
    deps = [
        ":order_job",
        "//game_arena/sandbox/exec:process_engine",
        "@googletest//:gtest_main",
    ],
)

cc_library(
    name = "order_outcome",
    srcs = ["order_outcome.cc"],
    hdrs = ["order_outcome.h"],
    deps = [
        ":build_log",
        ":grade_policy",
        ":match_tally",
        "//game_arena/common/metric_report",
        "//game_arena/proto:arena_cc_proto",
        "//game_arena/sandbox/common:text",
        "//game_arena/sandbox/exec:sandbox_job_cc_proto",
    ],
)

cc_test(
    name = "order_outcome_test",
    srcs = ["order_outcome_test.cc"],
    deps = [
        ":order_outcome",
        "@googletest//:gtest_main",
    ],
)

cc_library(
    name = "order_runner",
    srcs = ["order_runner.cc"],
    hdrs = ["order_runner.h"],
    deps = [
        ":order_job",
        "//game_arena/sandbox/exec:checkout",
        ":order_outcome",
        "//game_arena/proto:arena_cc_proto",
        "//game_arena/sandbox/exec:engine",
        "//game_arena/sandbox/exec:sandbox_job_cc_proto",
    ],
)



cc_binary(
    name = "sandbox_worker",
    srcs = ["worker_main.cc"],
    deps = [
        ":order_runner",
        "//game_arena/sandbox/exec:container_engine",
        "//game_arena/sandbox/exec:engine",
        "//game_arena/sandbox/exec:process_engine",
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

# End-to-end against fake `docker` and `git` scripts -- no daemon needed.
# Covers the mounts, the entrypoint scripts, host-side checkout, build failure
# reporting, run timeouts and patch-path validation.
cc_test(
    name = "order_runner_container_test",
    size = "medium",
    srcs = ["order_runner_container_test.cc"],
    deps = [
        ":order_runner",
        "//game_arena/sandbox/exec:container_engine",
        "@googletest//:gtest_main",
    ],
)

# The graded path end to end: fake git and bazel, but a real command writing a
# real report through $ARENA_REPORT.
cc_test(
    name = "order_runner_process_test",
    size = "medium",
    srcs = ["order_runner_process_test.cc"],
    deps = [
        ":order_runner",
        "//game_arena/sandbox/exec:process_engine",
        "@googletest//:gtest_main",
    ],
)
