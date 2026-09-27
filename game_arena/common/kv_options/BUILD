load("@rules_cc//cc:cc_library.bzl", "cc_library")
load("@rules_cc//cc:cc_test.bzl", "cc_test")

package(default_visibility = ["//visibility:public"])

cc_library(
    name = "kv_options",
    srcs = ["kv_options.cc"],
    hdrs = ["kv_options.h"],
    deps = ["@abseil-cpp//absl/strings"],
)

cc_test(
    name = "kv_options_test",
    srcs = ["kv_options_test.cc"],
    deps = [
        ":kv_options",
        "@googletest//:gtest_main",
    ],
)

# A kit vendors this package whole (//:kit_surface).
filegroup(
    name = "kit_surface_files",
    srcs = glob(["**"]),
)
