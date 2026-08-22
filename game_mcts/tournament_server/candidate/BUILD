load("@rules_cc//cc:cc_binary.bzl", "cc_binary")
load("@rules_cc//cc:cc_library.bzl", "cc_library")
load("@rules_cc//cc:cc_test.bzl", "cc_test")

package(default_visibility = ["//visibility:public"])

# The arena candidate contract: what an agent writes, and the harness that runs
# it. A candidate is one header defining MakePolicy(); everything else here is
# fixed and compiled unchanged around every submission.

# The one compiled piece of the contract. Separate because it is the only part
# that does not depend on which game was selected, so it is safe to compile
# once and link into candidates built for either game.
cc_library(
    name = "candidate_params",
    srcs = ["candidate_params.cc"],
    hdrs = ["candidate_params.h"],
)

# Header-only: game_t and kGameName resolve from CANDIDATE_GAME_*, so they must
# be compiled by the target that sets it, never inherited from a library.
cc_library(
    name = "candidate_api",
    hdrs = ["candidate_api.h"],
    deps = [
        ":candidate_params",
        "//game_mcts/cpp/mcts:tournament",
        "//game_mcts/cpp/risk:risk_game",
        "//game_mcts/cpp/risk:risk_serialization",
        "//game_mcts/cpp/risk/strategies:risk_proposer",
        "//game_mcts/cpp/risk/strategies:risk_rollout_shortcuts",
        "//game_mcts/cpp/tictactoe:tictactoe",
        "//game_mcts/cpp/tictactoe:tictactoe_serialization",
        "//game_mcts/tournament_server:policies",
    ],
)

# Exported as a source, not wrapped in a cc_library, because the per-candidate
# BUILD the sandbox worker generates has to compile it *itself*: the game and
# the entry header are chosen with local_defines, and those do not propagate
# from a dependency to its dependents.
exports_files(["candidate_main.cc"])

# The local iteration loop: builds cpp/tournament_server/candidates/dev against
# the same harness the arena uses, so what runs here is what gets submitted.
cc_binary(
    name = "dev_bot",
    srcs = ["candidate_main.cc"],
    local_defines = [
        r'CANDIDATE_ENTRY_HEADER=\"cpp/tournament_server/candidates/dev/strategy.h\"',
        "CANDIDATE_GAME_RISK2",
    ],
    deps = [
        ":candidate_api",
        "//game_mcts/tournament_server:remote_client",
        "//game_mcts/tournament_server:tournament_broker_cc_grpc",
        "//game_mcts/tournament_server/candidates/dev:strategy",
        "@abseil-cpp//absl/flags:flag",
        "@abseil-cpp//absl/flags:parse",
        "@abseil-cpp//absl/log",
        "@abseil-cpp//absl/log:initialize",
        "@grpc//:grpc++",
    ],
)

cc_test(
    name = "candidate_api_test",
    srcs = ["candidate_api_test.cc"],
    local_defines = ["CANDIDATE_GAME_TICTACTOE"],
    deps = [
        ":candidate_api",
        "@googletest//:gtest_main",
    ],
)
