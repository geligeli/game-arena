load("@rules_cc//cc:cc_library.bzl", "cc_library")
load("@rules_cc//cc:cc_test.bzl", "cc_test")

package(default_visibility = ["//visibility:public"])

# A kit gets this package and the referee links it, so nothing here may depend
# on //game_arena/server.
cc_library(
    name = "standings",
    hdrs = ["standings.h"],
    deps = [
        "//game_arena/proto:arena_cc_proto",
        "//game_arena/proto:tournament_broker_cc_proto",
    ],
)

# Read-only, so a kit need not carry the store that writes candidates.
cc_library(
    name = "candidate_view",
    hdrs = ["candidate_view.h"],
    deps = ["//game_arena/proto:arena_cc_proto"],
)

cc_library(
    name = "elo_store",
    srcs = ["elo_store.cc"],
    hdrs = ["elo_store.h"],
    deps = [
        "//game_arena/proto:tournament_broker_cc_proto",
        "@abseil-cpp//absl/log",
        ":game_history",
    ],
)

cc_library(
    name = "game_history",
    srcs = ["game_history.cc"],
    hdrs = ["game_history.h"],
    deps = [
        "//game_arena/proto:tournament_broker_cc_proto",
        "@abseil-cpp//absl/log",
        "@abseil-cpp//absl/strings",
        "@boost.json",
    ],
)

cc_library(
    name = "elo_standings",
    srcs = ["elo_standings.cc"],
    hdrs = ["elo_standings.h"],
    deps = [
        ":candidate_view",
        ":elo_store",
        ":standings",
        "@abseil-cpp//absl/log",
    ],
)

# The rating math alone: no dependencies.
cc_library(
    name = "trueskill",
    srcs = ["trueskill.cc"],
    hdrs = ["trueskill.h"],
)

cc_library(
    name = "trueskill_standings",
    srcs = ["trueskill_standings.cc"],
    hdrs = ["trueskill_standings.h"],
    deps = [
        ":candidate_view",
        ":game_history",
        ":standings",
        ":trueskill",
        "//game_arena/proto:tournament_broker_cc_proto",
        "@abseil-cpp//absl/log",
        "@boost.json",
    ],
)

cc_library(
    name = "metric_standings",
    srcs = ["metric_standings.cc"],
    hdrs = ["metric_standings.h"],
    deps = [
        ":standings",
        "@abseil-cpp//absl/log",
        "@abseil-cpp//absl/time",
        ":game_history",
    ],
)

cc_library(
    name = "http_leaderboard",
    srcs = ["http_leaderboard.cc"],
    hdrs = ["http_leaderboard.h"],
    deps = [
        ":candidate_view",
        ":game_history",
        ":standings",
        "@abseil-cpp//absl/log",
        "@abseil-cpp//absl/strings",
        "@abseil-cpp//absl/strings:str_format",
        "@boost.asio",
        "@boost.beast",
        "@boost.json",
        "//game_arena/referee:match_tally",
    ],
)

cc_test(
    name = "elo_store_test",
    srcs = ["elo_store_test.cc"],
    deps = [
        ":elo_store",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "trueskill_test",
    srcs = ["trueskill_test.cc"],
    deps = [
        ":trueskill",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "game_history_test",
    srcs = ["game_history_test.cc"],
    deps = [
        ":game_history",
        "@boost.json",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "http_leaderboard_test",
    srcs = ["http_leaderboard_test.cc"],
    deps = [
        ":http_leaderboard",
        "@boost.asio",
        "@boost.beast",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "standings_test",
    srcs = ["standings_test.cc"],
    deps = [
        ":elo_standings",
        ":game_history",
        ":metric_standings",
        ":trueskill_standings",
        "//game_arena/proto:tournament_broker_cc_proto",
        "@googletest//:gtest_main",
    ],
)

# A kit vendors this package whole (//:kit_surface).
filegroup(
    name = "kit_surface_files",
    srcs = glob(["**"]),
)
