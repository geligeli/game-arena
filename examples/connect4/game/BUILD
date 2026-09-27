load("@rules_cc//cc:cc_library.bzl", "cc_library")
load("@rules_cc//cc:cc_test.bzl", "cc_test")

package(default_visibility = ["//visibility:public"])

cc_library(
    name = "connect4",
    srcs = ["connect4.cc"],
    hdrs = ["connect4.h"],
    deps = ["@game_arena//game_arena/referee:game_session"],
)

# alwayslink: nothing depends on it by label; it defines a symbol the arena declares.
cc_library(
    name = "registry",
    srcs = ["registry.cc"],
    alwayslink = 1,
    deps = [
        ":connect4",
        "@game_arena//game_arena/referee:game_registry",
    ],
)

filegroup(
    name = "kit",
    srcs = glob([
        "BUILD",
        "*.cc",
        "*.h",
    ]),
)

cc_test(
    name = "connect4_test",
    srcs = ["connect4_test.cc"],
    deps = [
        ":connect4",
        ":registry",
        "@game_arena//game_arena/referee:game_registry",
        "@googletest//:gtest_main",
    ],
)

# A glob does not cross packages, so each exports its own (arena_problem's `tree`).
filegroup(
    name = "tree",
    srcs = glob(["**"]),
)
