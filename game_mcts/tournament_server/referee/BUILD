load("@rules_cc//cc:cc_binary.bzl", "cc_binary")
load("@rules_cc//cc:cc_library.bzl", "cc_library")
load("@rules_cc//cc:cc_test.bzl", "cc_test")

package(
    default_visibility = ["//visibility:public"],
)

# The referee: the game rules, the match loop, and the broker protocol the two
# sides of a match speak.
#
# Every problem-specific dependency in the tournament system lives here --
# game_registry alone pulls in risk, tictactoe and benchgame. This package is
# deliberately NOT a dependency of //game_mcts/tournament_server/server, which
# is a pure coordinator; that package's no_problem_code_test enforces it.
# Matches run here, in the sandbox, on a worker.

# Type-erased game engine: adapts any mcts::SerializableGame to a byte-level
# interface the broker core can drive.
cc_library(
    name = "game_session",
    srcs = ["game_session.cc"],
    hdrs = ["game_session.h"],
    deps = [
        "//game_mcts/core/mcts:game_traits",
        "//game_mcts/core/mcts:serialization",
        "//game_mcts/tournament_server/proto:tournament_broker_cc_proto",
        "@abseil-cpp//absl/log:check",
    ],
)

# Game-generic built-in strategies (random, minimax).
cc_library(
    name = "builtins",
    hdrs = ["builtins.h"],
    deps = [
        ":game_session",
        "//game_mcts/core/mcts:game_traits",
        "//game_mcts/core/mcts:minimax",
        "//game_mcts/core/mcts:serialization",
    ],
)

cc_library(
    name = "game_registry",
    srcs = ["game_registry.cc"],
    hdrs = ["game_registry.h"],
    deps = [
        ":builtins",
        ":game_session",
        "//game_mcts/core/mcts:game_traits",
        "//game_mcts/core/mcts:mcts",
        "//game_mcts/games/risk:risk_game",
        "//game_mcts/games/risk:risk_serialization",
        "//game_mcts/games/risk/strategies:risk_proposer",
        "//game_mcts/games/risk/strategies:risk_rollout_shortcuts",
        "//game_mcts/games/tictactoe:tictactoe",
        "//game_mcts/games/tictactoe:tictactoe_serialization",
        "//game_mcts/tournament_server/benchgame:bench_game",
        "//game_mcts/tournament_server/benchgame:bench_serialization",
    ],
)

cc_library(
    name = "client_handle",
    hdrs = ["client_handle.h"],
    deps = ["//game_mcts/tournament_server/proto:tournament_broker_cc_proto"],
)

# Bounded execution: a worker pool, per-game strands, and one timer thread.
# No gRPC dependency, so the game layer stays transport agnostic.
cc_library(
    name = "worker_pool",
    srcs = ["worker_pool.cc"],
    hdrs = ["worker_pool.h"],
    deps = ["@abseil-cpp//absl/functional:any_invocable"],
)

# One game as an event-driven state machine on a strand, instead of a blocking
# loop on a dedicated thread.
cc_library(
    name = "game_run",
    srcs = ["game_run.cc"],
    hdrs = ["game_run.h"],
    deps = [
        ":client_handle",
        "//game_mcts/tournament_server/server:elo_store",
        "//game_mcts/tournament_server/server:game_history",
        ":game_session",
        ":worker_pool",
        "//game_mcts/tournament_server/proto:tournament_broker_cc_proto",
        "@abseil-cpp//absl/log",
    ],
)

cc_library(
    name = "matchmaker",
    srcs = ["matchmaker.cc"],
    hdrs = ["matchmaker.h"],
    deps = [
        ":client_handle",
        "//game_mcts/tournament_server/server:elo_store",
        "//game_mcts/tournament_server/server:game_history",
        ":game_registry",
        ":game_run",
        ":game_session",
        ":worker_pool",
        "//game_mcts/tournament_server/proto:tournament_broker_cc_proto",
        "@abseil-cpp//absl/log",
    ],
)

# ClientHandle over a gRPC bidi stream, split from the reactor that owns the
# RPC so the two lifetimes can differ.
cc_library(
    name = "player_connection",
    srcs = ["player_connection.cc"],
    hdrs = ["player_connection.h"],
    deps = [
        ":client_handle",
        "//game_mcts/tournament_server/proto:tournament_broker_cc_proto",
        "@abseil-cpp//absl/log",
        "@grpc//:grpc++",
    ],
)

# Callback-API reactor: one per Play stream, owned by gRPC.
cc_library(
    name = "play_reactor",
    srcs = ["play_reactor.cc"],
    hdrs = ["play_reactor.h"],
    deps = [
        ":matchmaker",
        ":player_connection",
        "//game_mcts/tournament_server/proto:tournament_broker_cc_grpc",
        "@abseil-cpp//absl/log",
        "@grpc//:grpc++",
    ],
)

cc_library(
    name = "broker_service",
    srcs = ["broker_service.cc"],
    hdrs = ["broker_service.h"],
    deps = [
        ":matchmaker",
        ":play_reactor",
        "//game_mcts/tournament_server/proto:tournament_broker_cc_grpc",
        "@grpc//:grpc++",
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

cc_test(
    name = "game_run_test",
    srcs = ["game_run_test.cc"],
    deps = [
        ":client_handle",
        "//game_mcts/tournament_server/server:elo_store",
        "//game_mcts/tournament_server/server:game_history",
        ":game_registry",
        ":game_run",
        ":worker_pool",
        "//game_mcts/games/tictactoe:tictactoe",
        "//game_mcts/games/tictactoe:tictactoe_cc_proto",
        "//game_mcts/games/tictactoe:tictactoe_serialization",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "game_session_test",
    srcs = ["game_session_test.cc"],
    deps = [
        ":game_registry",
        ":game_session",
        "//game_mcts/games/risk:risk_game",
        "//game_mcts/games/risk:risk_serialization",
        "//game_mcts/games/risk/strategies:risk_proposer",
        "//game_mcts/games/tictactoe:tictactoe",
        "//game_mcts/games/tictactoe:tictactoe_cc_proto",
        "//game_mcts/games/tictactoe:tictactoe_serialization",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "broker_integration_test",
    srcs = ["broker_integration_test.cc"],
    deps = [
        "//game_mcts/tournament_server/server:elo_standings",
        ":broker_service",
        ":client_handle",
        "//game_mcts/tournament_server/server:elo_store",
        "//game_mcts/tournament_server/server:game_history",
        ":game_registry",
        "//game_mcts/tournament_server/server:http_leaderboard",
        ":matchmaker",
        "//game_mcts/games/tictactoe:tictactoe",
        "//game_mcts/games/tictactoe:tictactoe_serialization",
        "//game_mcts/tournament_server/client:remote_client",
        "//game_mcts/tournament_server/proto:tournament_broker_cc_grpc",
        "@googletest//:gtest_main",
        "@grpc//:grpc++",
    ],
)

# One match, then exit. Started by a sandbox worker per order; see main.cc.
cc_binary(
    name = "match_referee",
    srcs = ["main.cc"],
    deps = [
        ":broker_service",
        ":game_registry",
        ":matchmaker",
        "//game_mcts/tournament_server/proto:tournament_broker_cc_proto",
        "//game_mcts/tournament_server/server:elo_store",
        "//game_mcts/tournament_server/server:game_history",
        "@abseil-cpp//absl/flags:flag",
        "@abseil-cpp//absl/flags:parse",
        "@abseil-cpp//absl/log",
        "@abseil-cpp//absl/log:globals",
        "@abseil-cpp//absl/log:initialize",
        "@grpc//:grpc++",
    ],
)

# The long-running broker, for the local development loop: point dev_bot or
# random_client at it and iterate without an arena or a container. Not on the
# arena's evaluation path -- that is :match_referee, one match at a time.
cc_binary(
    name = "broker_server",
    srcs = ["broker_main.cc"],
    deps = [
        ":broker_service",
        ":game_registry",
        ":matchmaker",
        "//game_mcts/tournament_server/server:elo_standings",
        "//game_mcts/tournament_server/server:elo_store",
        "//game_mcts/tournament_server/server:game_history",
        "//game_mcts/tournament_server/server:http_leaderboard",
        "@abseil-cpp//absl/flags:flag",
        "@abseil-cpp//absl/flags:parse",
        "@abseil-cpp//absl/log",
        "@abseil-cpp//absl/log:globals",
        "@abseil-cpp//absl/log:initialize",
        "@grpc//:grpc++",
    ],
)
