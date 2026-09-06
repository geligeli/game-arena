load("@rules_cc//cc:cc_binary.bzl", "cc_binary")
load("@rules_cc//cc:cc_library.bzl", "cc_library")

package(
    default_visibility = ["//visibility:public"],
)

# Client-side strategy code: everything that runs in a player's process and
# talks to the broker over the Play stream.

# Stock TournamentPolicy adapters (ProposerPolicy, MctsPolicy). Split from
# remote_client so a strategy can be defined without pulling in gRPC.
cc_library(
    name = "policies",
    hdrs = ["policies.h"],
    deps = [
        "//game_mcts/core/mcts:game_traits",
        "//game_mcts/core/mcts:mcts",
        "//game_mcts/core/mcts:tournament",
    ],
)

# Client-side strategy wrapper: PlayRemoteGames joins any TournamentPolicy<G>
# to a broker; ProposerPolicy and MctsPolicy come with it via policies.h.
cc_library(
    name = "remote_client",
    hdrs = ["remote_client.h"],
    deps = [
        ":policies",
        "//game_mcts/core/mcts:game_traits",
        "//game_mcts/core/mcts:mcts",
        "//game_mcts/core/mcts:serialization",
        "//game_mcts/core/mcts:tournament",
        "//game_mcts/tournament_server/proto:tournament_broker_cc_grpc",
        "@abseil-cpp//absl/log",
        "@grpc//:grpc++",
    ],
)

cc_binary(
    name = "random_client",
    srcs = ["random_client.cc"],
    deps = [
        "//game_mcts/tournament_server/proto:tournament_broker_cc_grpc",
        "//game_mcts/tournament_server/referee:game_registry",
        "@abseil-cpp//absl/flags:flag",
        "@abseil-cpp//absl/flags:parse",
        "@abseil-cpp//absl/log",
        "@abseil-cpp//absl/log:initialize",
        "@grpc//:grpc++",
    ],
)

cc_binary(
    name = "risk_mcts_client",
    srcs = ["risk_mcts_client.cc"],
    deps = [
        ":remote_client",
        "//game_mcts/games/risk:risk_game",
        "//game_mcts/games/risk:risk_serialization",
        "//game_mcts/games/risk/strategies:risk_proposer",
        "//game_mcts/games/risk/strategies:risk_rollout_shortcuts",
        "//game_mcts/tournament_server/proto:tournament_broker_cc_grpc",
        "@abseil-cpp//absl/flags:flag",
        "@abseil-cpp//absl/flags:parse",
        "@abseil-cpp//absl/log",
        "@abseil-cpp//absl/log:initialize",
        "@grpc//:grpc++",
    ],
)
