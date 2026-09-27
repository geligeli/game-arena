load("@rules_cc//cc:cc_library.bzl", "cc_library")
load("@rules_cc//cc:cc_test.bzl", "cc_test")

package(default_visibility = ["//visibility:public"])

cc_library(
    name = "files",
    srcs = ["files.cc"],
    hdrs = ["files.h"],
)

cc_test(
    name = "files_test",
    srcs = ["files_test.cc"],
    deps = [
        ":files",
        "@googletest//:gtest_main",
    ],
)

cc_library(
    name = "text",
    srcs = ["text.cc"],
    hdrs = ["text.h"],
)

cc_test(
    name = "text_test",
    srcs = ["text_test.cc"],
    deps = [
        ":text",
        "@googletest//:gtest_main",
    ],
)

cc_library(
    name = "step",
    srcs = ["step.cc"],
    hdrs = ["step.h"],
    deps = [
        ":files",
        "//game_arena/common/process",
    ],
)

cc_test(
    name = "step_test",
    srcs = ["step_test.cc"],
    deps = [
        ":files",
        ":step",
        "@googletest//:gtest_main",
    ],
)

cc_library(
    name = "docker",
    srcs = ["docker.cc"],
    hdrs = ["docker.h"],
)

cc_test(
    name = "docker_test",
    srcs = ["docker_test.cc"],
    deps = [
        ":docker",
        "@googletest//:gtest_main",
    ],
)
