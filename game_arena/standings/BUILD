load("@rules_cc//cc:cc_library.bzl", "cc_library")
load("@rules_cc//cc:cc_test.bzl", "cc_test")

package(
    default_visibility = ["//visibility:public"],
)

# How a tournament's results are rated, kept and shown.
#
# Its own package because two very different things link it: the coordinator,
# which owns the authoritative standings, and the referee -- a worker's, or
# the one `arena_cli spar` runs in a kit. That second one is why
# nothing here may reach into //game_arena/server -- a kit gets this package
# and not the coordinator, and the dependency edge is what keeps that true.
#
#   bazel query 'somepath(//game_arena/standings/..., //game_arena/server/...)'
#
# must stay empty.

# The score a problem is ordered by, whatever it is: an ELO rating or a
# measured metric, behind one interface so the scheduler, the Arena service
# and the leaderboard need no branch.
cc_library(
    name = "standings",
    hdrs = ["standings.h"],
    deps = ["//game_arena/proto:arena_cc_proto"],
)

# Reading candidates, for the things that only read them: the standings and
# the leaderboard. Depending on the store that writes them would drag the
# coordinator's submission machinery -- the patch validator, the BUILD
# generator -- into every kit.
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
    ],
)

cc_library(
    name = "game_history",
    srcs = ["game_history.cc"],
    hdrs = ["game_history.h"],
    deps = [
        "//game_arena/proto:tournament_broker_cc_proto",
        "@abseil-cpp//absl/log",
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

cc_library(
    name = "metric_standings",
    srcs = ["metric_standings.cc"],
    hdrs = ["metric_standings.h"],
    deps = [
        ":standings",
        "@abseil-cpp//absl/log",
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
        ":metric_standings",
        "@googletest//:gtest_main",
    ],
)

# Every file of this package, for the arena a participant's kit builds
# against: a kit gets these packages whole (see //:kit_surface).
filegroup(
    name = "kit_surface_files",
    srcs = glob(["**"]),
)
