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
