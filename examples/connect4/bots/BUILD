load("@rules_cc//cc:cc_binary.bzl", "cc_binary")
load("@rules_cc//cc:cc_library.bzl", "cc_library")

package(default_visibility = ["//visibility:public"])

# The submission contract: what an entrant writes, and the harness compiled
# unchanged around it.

# The board type and helpers a strategy is written against.
cc_library(
    name = "bot_api",
    hdrs = ["bot_api.h"],
    deps = ["//game:connect4"],
)

# Exported as a source, not wrapped in a cc_library, because the per-submission
# BUILD the coordinator generates has to compile it *itself*: the entry header
# arrives as a local_define, and those do not propagate from a dependency.
exports_files(["bot_main.cc"])

# What the generated per-submission binary links. Named here so
# problem.textproto's submission.harness can point at one label instead of
# repeating the list.
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

cc_library(
    name = "reference_strategy",
    hdrs = ["reference/strategy.h"],
    deps = [":bot_api"],
)

# The local iteration loop: the same harness the arena compiles, around the
# reference strategy. Point it at a broker and watch it play.
cc_binary(
    name = "dev_bot",
    srcs = ["bot_main.cc"],
    local_defines = [
        r'CANDIDATE_ENTRY_HEADER=\"bots/reference/strategy.h\"',
    ],
    deps = [
        ":bot_deps",
        ":reference_strategy",
    ],
)

# What a participant receives: the API, the harness compiled around every
# submission, and the reference strategy to start from.
filegroup(
    name = "kit",
    srcs = glob([
        "BUILD",
        "*.cc",
        "*.h",
        "reference/*",
    ]),
)
