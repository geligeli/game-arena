load("@rules_cc//cc:cc_library.bzl", "cc_library")
load("@rules_cc//cc:cc_test.bzl", "cc_test")

package(default_visibility = ["//visibility:public"])

# The rules. Depends on one arena header -- the GameSession contract -- and
# nothing else.
cc_library(
    name = "connect4",
    srcs = ["connect4.cc"],
    hdrs = ["connect4.h"],
    deps = ["@game_arena//game_arena/referee:game_session"],
)

# The definition of GameRegistry(). alwayslink because nothing depends on it by
# label: it exists to satisfy a symbol the arena leaves undefined.
cc_library(
    name = "registry",
    srcs = ["registry.cc"],
    alwayslink = 1,
    deps = [
        ":connect4",
        "@game_arena//game_arena/referee:game_registry",
    ],
)

# Everything a participant needs from this package, for the kit that
# `bazel run //:kit` writes. The referee is assembled at the root by
# arena_problem(); here are only the rules.
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
