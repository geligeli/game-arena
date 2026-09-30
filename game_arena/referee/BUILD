load("@rules_cc//cc:cc_library.bzl", "cc_library")
load("@rules_cc//cc:cc_test.bzl", "cc_test")

package(default_visibility = ["//visibility:public"])

# Game-agnostic: states and actions are only ever byte strings.
cc_library(
    name = "game_session",
    srcs = ["game_session.cc"],
    hdrs = ["game_session.h"],
    deps = ["@abseil-cpp//absl/time"],
)

# Declared, never defined here: a binary links a registry (//game_arena/testgame).
cc_library(
    name = "game_registry",
    hdrs = ["game_registry.h"],
    deps = [":game_session"],
)

cc_library(
    name = "client_handle",
    hdrs = ["client_handle.h"],
    deps = [
        "//game_arena/proto:tournament_broker_cc_proto",
        "//game_arena/standings:game_history",
    ],
)

# No gRPC below play_reactor: the game layer stays transport agnostic.
cc_library(
    name = "worker_pool",
    srcs = ["worker_pool.cc"],
    hdrs = ["worker_pool.h"],
    deps = ["@abseil-cpp//absl/functional:any_invocable"],
)

cc_library(
    name = "game_run",
    srcs = ["game_run.cc"],
    hdrs = ["game_run.h"],
    deps = [
        ":client_handle",
        ":game_session",
        ":worker_pool",
        "//game_arena/proto:tournament_broker_cc_proto",
        "//game_arena/standings:game_history",
        "@abseil-cpp//absl/log",
        "@abseil-cpp//absl/time",
        "@abseil-cpp//absl/strings",
    ],
)

cc_library(
    name = "matchmaker",
    srcs = ["matchmaker.cc"],
    hdrs = ["matchmaker.h"],
    deps = [
        ":client_handle",
        ":game_registry",
        ":game_run",
        ":game_session",
        ":worker_pool",
        "//game_arena/proto:tournament_broker_cc_proto",
        "//game_arena/standings:game_history",
        "@abseil-cpp//absl/log",
        "@abseil-cpp//absl/strings",
    ],
)

cc_library(
    name = "player_connection",
    srcs = ["player_connection.cc"],
    hdrs = ["player_connection.h"],
    deps = [
        ":client_handle",
        "//game_arena/proto:tournament_broker_cc_proto",
        "@abseil-cpp//absl/log",
        "@grpc//:grpc++",
    ],
)

cc_library(
    name = "play_reactor",
    srcs = ["play_reactor.cc"],
    hdrs = ["play_reactor.h"],
    deps = [
        ":matchmaker",
        ":player_connection",
        "//game_arena/proto:tournament_broker_cc_grpc",
        "@abseil-cpp//absl/log",
        "@grpc//:grpc++",
        "@abseil-cpp//absl/strings",
    ],
)

cc_library(
    name = "match_tally",
    srcs = ["match_tally.cc"],
    hdrs = ["match_tally.h"],
    deps = [
        "//game_arena/proto:tournament_broker_cc_proto",
        "//game_arena/standings:game_history",
    ],
)

cc_test(
    name = "match_tally_test",
    srcs = ["match_tally_test.cc"],
    deps = [
        ":match_tally",
        "@googletest//:gtest_main",
    ],
)

# A library, to be linked with a registry; alwayslink keeps main().
cc_library(
    name = "referee_main",
    srcs = ["referee_main.cc"],
    alwayslink = 1,
    deps = [
        "//game_arena/common/kv_options",
        ":game_registry",
        ":match_tally",
        ":matchmaker",
        ":play_reactor",
        "//game_arena/proto:tournament_broker_cc_proto",
        "//game_arena/standings:game_history",
        "@abseil-cpp//absl/flags:flag",
        "@abseil-cpp//absl/flags:parse",
        "@abseil-cpp//absl/log",
        "@abseil-cpp//absl/log:globals",
        "@abseil-cpp//absl/log:initialize",
        "@grpc//:grpc++",
        "@abseil-cpp//absl/strings",
    ],
)

cc_test(
    name = "player_connection_test",
    srcs = ["player_connection_test.cc"],
    deps = [
        ":player_connection",
        "@googletest//:gtest_main",
        "@grpc//:grpc++",
    ],
)

cc_test(
    name = "worker_pool_test",
    srcs = ["worker_pool_test.cc"],
    deps = [
        ":worker_pool",
        "@googletest//:gtest_main",
    ],
)

# A kit vendors this package whole (//:kit_surface).
filegroup(
    name = "kit_surface_files",
    srcs = glob(["**"]),
)
