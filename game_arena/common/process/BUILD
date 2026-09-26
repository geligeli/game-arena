load("@rules_cc//cc:cc_library.bzl", "cc_library")
load("@rules_cc//cc:cc_test.bzl", "cc_test")

cc_library(
    name = "process",
    srcs = [
        "process.cpp",
    ],
    hdrs = [
        "process.h",
    ],
    visibility = ["//visibility:public"],
)

cc_test(
    name = "process_test",
    srcs = [
        "process_test.cpp",
    ],
    deps = [
        ":process",
        "@googletest//:gtest_main",
    ],
)

# Every file of this package, for the arena a sandbox builds against: a C++
# grader runs the solution with it (see //:sandbox_surface).
filegroup(
    name = "sandbox_surface_files",
    srcs = glob(["**"]),
    visibility = ["//visibility:public"],
)
