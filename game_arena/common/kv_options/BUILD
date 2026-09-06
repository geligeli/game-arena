load("@rules_cc//cc:cc_library.bzl", "cc_library")
load("@rules_cc//cc:cc_test.bzl", "cc_test")

package(default_visibility = ["//visibility:public"])

cc_library(
    name = "kv_options",
    srcs = ["kv_options.cc"],
    hdrs = ["kv_options.h"],
)

cc_test(
    name = "kv_options_test",
    srcs = ["kv_options_test.cc"],
    deps = [
        ":kv_options",
        "@googletest//:gtest_main",
    ],
)
