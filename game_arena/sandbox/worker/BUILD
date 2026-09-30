load("@rules_cc//cc:cc_binary.bzl", "cc_binary")
load("@rules_cc//cc:cc_library.bzl", "cc_library")
load("@rules_cc//cc:cc_test.bzl", "cc_test")

package(default_visibility = ["//visibility:public"])

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

cc_library(
    name = "build_log",
    srcs = ["build_log.cc"],
    hdrs = ["build_log.h"],
    deps = [
        "//game_arena/sandbox/common:text",
        "@re2",
    ],
)

cc_library(
    name = "order_job",
    srcs = ["order_job.cc"],
    hdrs = ["order_job.h"],
    deps = [
        "//game_arena/common/kv_options",
        "//game_arena/proto:arena_cc_proto",
        "//game_arena/sandbox/common:docker",
        "//game_arena/sandbox/exec:engine",
        "//game_arena/sandbox/exec:sandbox_job_cc_proto",
        "@abseil-cpp//absl/strings",
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
        ":order_job",
        "//game_arena/common/metric_report",
        "//game_arena/proto:arena_cc_proto",
        "//game_arena/proto:tournament_broker_cc_proto",
        "//game_arena/referee:match_tally",
        "//game_arena/sandbox/common:text",
        "//game_arena/sandbox/exec:sandbox_job_cc_proto",
    ],
)

cc_test(
    name = "order_outcome_test",
    srcs = ["order_outcome_test.cc"],
    deps = [
        ":order_job",
        ":order_outcome",
        "@googletest//:gtest_main",
    ],
)

cc_library(
    name = "artifact_cache",
    srcs = ["artifact_cache.cc"],
    hdrs = ["artifact_cache.h"],
    deps = [
        "//game_arena/common/sha256",
        "//game_arena/proto:arena_cc_grpc",
        "@grpc//:grpc++",
    ],
)

cc_library(
    name = "order_runner",
    srcs = ["order_runner.cc"],
    hdrs = ["order_runner.h"],
    deps = [
        ":artifact_cache",
        ":order_job",
        ":order_outcome",
        "//game_arena/proto:arena_cc_proto",
        "//game_arena/sandbox/exec:engine",
        "//game_arena/sandbox/exec:sandbox_job_cc_proto",
    ],
)



# The container engine and no other: there is no unsandboxed backend.
cc_binary(
    name = "sandbox_worker",
    srcs = ["sandbox_worker.cc"],
    deps = [
        ":artifact_cache",
        ":order_runner",
        "//game_arena/common/process",
        "//game_arena/sandbox/exec:container_engine",
        "//game_arena/proto:arena_cc_grpc",
        "@abseil-cpp//absl/flags:flag",
        "@abseil-cpp//absl/flags:parse",
        "@abseil-cpp//absl/log",
        "@abseil-cpp//absl/log:globals",
        "@abseil-cpp//absl/log:initialize",
        "@grpc//:grpc++",
        "@abseil-cpp//absl/strings",
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

# Against fake `docker` and `git` scripts; no daemon needed.
cc_test(
    name = "order_runner_container_test",
    size = "medium",
    srcs = ["order_runner_container_test.cc"],
    deps = [
        ":artifact_cache",
        "//game_arena/proto:arena_cc_grpc",
        "@grpc//:grpc++",
        ":order_runner",
        "//game_arena/sandbox/exec:container_engine",
        "@googletest//:gtest_main",
    ],
)

# Fake git and bazel, and a real command writing a real $ARENA_REPORT.
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
