load("@rules_cc//cc:cc_library.bzl", "cc_library")

package(default_visibility = ["//visibility:public"])

cc_library(
    name = "bot_api",
    hdrs = ["bot_api.h"],
    deps = ["//game:connect4"],
)

# A source, not a cc_library: the generated BUILD compiles it with the entry
# header as a local_define, which does not propagate from a dependency.
exports_files(["bot.cc"])

# What the generated per-submission binary links (submission.harness.bot_deps).
cc_library(
    name = "bot_deps",
    deps = [
        ":bot_api",
        "//game:connect4",
        "@abseil-cpp//absl/flags:flag",
        "@abseil-cpp//absl/flags:parse",
        "@abseil-cpp//absl/log",
        "@abseil-cpp//absl/log:globals",
        "@abseil-cpp//absl/log:initialize",
        "@game_arena//game_arena/client:play_loop",
        "@game_arena//game_arena/proto:tournament_broker_cc_grpc",
        "@grpc//:grpc++",
    ],
)

# Each participant is a directory here, with the BUILD the arena generates.
filegroup(
    name = "kit",
    srcs = glob([
        "BUILD",
        "*.cc",
        "*.h",
    ]),
)

# A glob does not cross packages, so each exports its own (arena_problem's `tree`).
filegroup(
    name = "tree",
    srcs = glob(["**"]),
)
