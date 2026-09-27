load("@rules_cc//cc:cc_library.bzl", "cc_library")
load("@rules_cc//cc:cc_test.bzl", "cc_test")

package(default_visibility = ["//visibility:public"])

# For C++ graders; one in any other language can printf the JSON.
cc_library(
    name = "report",
    srcs = ["report.cc"],
    hdrs = ["report.h"],
    deps = ["@boost.json"],
)

cc_test(
    name = "report_test",
    srcs = ["report_test.cc"],
    deps = [
        ":report",
        "//game_arena/common/metric_report",
        "@googletest//:gtest_main",
    ],
)

# A sandbox vendors this package whole (//:sandbox_surface).
filegroup(
    name = "sandbox_surface_files",
    srcs = glob(["**"]),
)
