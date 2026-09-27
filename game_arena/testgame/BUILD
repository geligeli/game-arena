load("@rules_cc//cc:cc_binary.bzl", "cc_binary")
load("@rules_cc//cc:cc_library.bzl", "cc_library")
load("@rules_cc//cc:cc_test.bzl", "cc_test")

package(default_visibility = ["//visibility:public"])

# Nim, the arena's own game, and the reference assembly of a referee.

cc_library(
    name = "nim",
    srcs = ["nim.cc"],
    hdrs = ["nim.h"],
    deps = [
        "//game_arena/referee:game_session",
        "@abseil-cpp//absl/log:check",
    ],
)

# Defines GameRegistry(); alwayslink, as nothing depends on it by label.
cc_library(
    name = "game_registry",
    srcs = ["game_registry.cc"],
    alwayslink = 1,
    deps = [
        ":nim",
        "//game_arena/referee:game_registry",
    ],
)

cc_binary(
    name = "match_referee",
    deps = [
        ":game_registry",
        "//game_arena/referee:referee_main",
    ],
)

cc_test(
    name = "nim_test",
    srcs = ["nim_test.cc"],
    deps = [
        ":game_registry",
        ":nim",
        "//game_arena/referee:game_registry",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "nim_game_run_test",
    srcs = ["nim_game_run_test.cc"],
    deps = [
        ":game_registry",
        ":nim",
        "//game_arena/referee:client_handle",
        "//game_arena/referee:game_registry",
        "//game_arena/referee:game_run",
        "//game_arena/referee:matchmaker",
        "//game_arena/referee:worker_pool",
        "//game_arena/standings:game_history",
        "@googletest//:gtest_main",
    ],
)
