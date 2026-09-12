load("@rules_cc//cc:cc_library.bzl", "cc_library")
load("@rules_cc//cc:cc_test.bzl", "cc_test")

package(default_visibility = ["//visibility:public"])

# For graders that are C++ binaries. A grader in any other language should just
# printf the JSON; this is not a dependency the arena imposes on a problem.
cc_library(
    name = "report",
    srcs = ["report.cc"],
    hdrs = ["report.h"],
)

# Round trips through the worker's real parser: the two halves of this format
# live in one repo precisely so a test can hold them together.
cc_test(
    name = "report_test",
    srcs = ["report_test.cc"],
    deps = [
        ":report",
        "//game_arena/common/metric_report",
        "@googletest//:gtest_main",
    ],
)
