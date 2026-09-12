load("@rules_cc//cc:cc_library.bzl", "cc_library")
load("@rules_cc//cc:cc_test.bzl", "cc_test")

package(default_visibility = ["//visibility:public"])

# The $ARENA_REPORT format, as data. Both ends of it live in this repo on
# purpose -- game_arena/grader writes it, a sandbox worker reads it back -- so
# it sits below both rather than inside either. What to do with several runs'
# numbers is a separate, arena-shaped question: see
# //game_arena/sandbox/worker:grade_policy.
cc_library(
    name = "metric_report",
    srcs = ["metric_report.cc"],
    hdrs = ["metric_report.h"],
    deps = [
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
