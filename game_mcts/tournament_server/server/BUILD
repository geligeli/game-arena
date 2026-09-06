load("@rules_cc//cc:cc_binary.bzl", "cc_binary")
load("@rules_cc//cc:cc_library.bzl", "cc_library")
load("@rules_cc//cc:cc_test.bzl", "cc_test")

package(
    default_visibility = ["//visibility:public"],
)

# The broker process: gRPC services, the game/session domain, matchmaking,
# scheduling, ELO and history stores, and the HTTP leaderboard. Client-side
# strategy code lives in //game_mcts/tournament_server/client.

# Persistent registry of submitted strategies: manifests plus their sources on
# disk, and the validation gate every submission passes through.
cc_library(
    name = "candidate_store",
    srcs = ["candidate_store.cc"],
    hdrs = ["candidate_store.h"],
    deps = [
        "//game_mcts/tournament_server/proto:arena_cc_proto",
        "@abseil-cpp//absl/log",
    ],
)

cc_test(
    name = "candidate_store_test",
    srcs = ["candidate_store_test.cc"],
    deps = [
        ":candidate_store",
        "@googletest//:gtest_main",
    ],
)

# One attached sandbox worker, as the scheduler sees it. Kept gRPC-free so the
# scheduler can be tested with fakes.
cc_library(
    name = "fleet_worker",
    hdrs = ["fleet_worker.h"],
    deps = ["//game_mcts/tournament_server/proto:arena_cc_proto"],
)

# Expands "rate this candidate" into work orders and hands them to the fleet.
cc_library(
    name = "scheduler",
    srcs = ["scheduler.cc"],
    hdrs = ["scheduler.h"],
    deps = [
        ":candidate_store",
        ":elo_store",
        ":fleet_worker",
        "//game_mcts/tournament_server/proto:arena_cc_proto",
        "@abseil-cpp//absl/log",
    ],
)

cc_test(
    name = "scheduler_test",
    srcs = ["scheduler_test.cc"],
    deps = [
        ":scheduler",
        "@googletest//:gtest_main",
    ],
)

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
    name = "elo_store",
    srcs = ["elo_store.cc"],
    hdrs = ["elo_store.h"],
    deps = [
        "//game_mcts/tournament_server/proto:tournament_broker_cc_proto",
        "@abseil-cpp//absl/log",
    ],
)

cc_library(
    name = "game_history",
    srcs = ["game_history.cc"],
    hdrs = ["game_history.h"],
    deps = [
        "//game_mcts/tournament_server/proto:tournament_broker_cc_proto",
        "@abseil-cpp//absl/log",
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
        ":elo_store",
        ":game_history",
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
        ":elo_store",
        ":game_history",
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

# Agent-facing arena RPCs, consumed by the MCP server.
cc_library(
    name = "arena_service",
    srcs = ["arena_service.cc"],
    hdrs = ["arena_service.h"],
    deps = [
        ":candidate_store",
        ":elo_store",
        ":scheduler",
        "//game_mcts/tournament_server/proto:arena_cc_grpc",
        "@abseil-cpp//absl/log",
        "@grpc//:grpc++",
    ],
)

# Sandbox fleet RPCs: one Attach stream per worker host.
cc_library(
    name = "fleet_service",
    srcs = ["fleet_service.cc"],
    hdrs = ["fleet_service.h"],
    deps = [
        ":fleet_worker",
        ":scheduler",
        "//game_mcts/tournament_server/proto:arena_cc_grpc",
        "@abseil-cpp//absl/log",
        "@grpc//:grpc++",
    ],
)

cc_library(
    name = "http_leaderboard",
    srcs = ["http_leaderboard.cc"],
    hdrs = ["http_leaderboard.h"],
    deps = [
        ":candidate_store",
        ":elo_store",
        ":game_history",
        "@abseil-cpp//absl/log",
    ],
)

cc_binary(
    name = "tournament_server",
    srcs = ["main.cc"],
    deps = [
        ":arena_service",
        ":broker_service",
        ":candidate_store",
        ":elo_store",
        ":fleet_service",
        ":game_history",
        ":game_registry",
        ":http_leaderboard",
        ":matchmaker",
        ":scheduler",
        "@abseil-cpp//absl/flags:flag",
        "@abseil-cpp//absl/flags:parse",
        "@abseil-cpp//absl/log",
        "@abseil-cpp//absl/log:globals",
        "@abseil-cpp//absl/log:initialize",
        "@grpc//:grpc++",
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
        ":elo_store",
        ":game_history",
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
    name = "arena_integration_test",
    srcs = ["arena_integration_test.cc"],
    deps = [
        ":arena_service",
        ":candidate_store",
        ":elo_store",
        ":fleet_service",
        ":scheduler",
        "//game_mcts/tournament_server/proto:arena_cc_grpc",
        "@googletest//:gtest_main",
        "@grpc//:grpc++",
    ],
)

cc_test(
    name = "broker_integration_test",
    srcs = ["broker_integration_test.cc"],
    deps = [
        ":broker_service",
        ":client_handle",
        ":elo_store",
        ":game_history",
        ":game_registry",
        ":http_leaderboard",
        ":matchmaker",
        "//game_mcts/games/tictactoe:tictactoe",
        "//game_mcts/games/tictactoe:tictactoe_serialization",
        "//game_mcts/tournament_server/client:remote_client",
        "//game_mcts/tournament_server/proto:tournament_broker_cc_grpc",
        "@googletest//:gtest_main",
        "@grpc//:grpc++",
    ],
)
