load("@rules_cc//cc:cc_library.bzl", "cc_library")
load("@rules_cc//cc:cc_test.bzl", "cc_test")

package(default_visibility = ["//visibility:public"])

# Below both ends of the format: game_arena/grader and the sandbox worker.
cc_library(
    name = "metric_report",
    srcs = ["metric_report.cc"],
    hdrs = ["metric_report.h"],
    deps = [
        "//game_arena/proto:problem_cc_proto",
        "@abseil-cpp//absl/strings",
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
