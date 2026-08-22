load("@grpc//bazel:cc_grpc_library.bzl", "cc_grpc_library")
load("@protobuf//bazel:cc_proto_library.bzl", "cc_proto_library")
load("@protobuf//bazel:proto_library.bzl", "proto_library")
load("@rules_cc//cc:cc_binary.bzl", "cc_binary")
load("@rules_cc//cc:cc_library.bzl", "cc_library")
load("@rules_cc//cc:cc_test.bzl", "cc_test")

package(
    default_visibility = ["//visibility:public"],
)

proto_library(
    name = "tournament_broker_proto",
    srcs = ["tournament_broker.proto"],
)

cc_proto_library(
    name = "tournament_broker_cc_proto",
    deps = [":tournament_broker_proto"],
)

cc_grpc_library(
    name = "tournament_broker_cc_grpc",
    srcs = [":tournament_broker_proto"],
    grpc_only = True,
    deps = [":tournament_broker_cc_proto"],
)

proto_library(
    name = "arena_proto",
    srcs = ["arena.proto"],
)

cc_proto_library(
    name = "arena_cc_proto",
    deps = [":arena_proto"],
)

cc_grpc_library(
    name = "arena_cc_grpc",
    srcs = [":arena_proto"],
    grpc_only = True,
    deps = [":arena_cc_proto"],
)

# Persistent registry of submitted strategies: manifests plus their sources on
# disk, and the validation gate every submission passes through.
cc_library(
    name = "candidate_store",
    srcs = ["candidate_store.cc"],
    hdrs = ["candidate_store.h"],
    deps = [
        ":arena_cc_proto",
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
    deps = [":arena_cc_proto"],
)

# Expands "rate this candidate" into work orders and hands them to the fleet.
cc_library(
    name = "scheduler",
    srcs = ["scheduler.cc"],
    hdrs = ["scheduler.h"],
    deps = [
        ":arena_cc_proto",
        ":candidate_store",
        ":elo_store",
        ":fleet_worker",
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
        ":tournament_broker_cc_proto",
        "//game_mcts/cpp/mcts:game_traits",
        "//game_mcts/cpp/mcts:serialization",
        "@abseil-cpp//absl/log:check",
    ],
)

# Game-generic built-in strategies (random, minimax).
cc_library(
    name = "builtins",
    hdrs = ["builtins.h"],
    deps = [
        ":game_session",
        "//game_mcts/cpp/mcts:game_traits",
        "//game_mcts/cpp/mcts:minimax",
        "//game_mcts/cpp/mcts:serialization",
    ],
)

cc_library(
    name = "game_registry",
    srcs = ["game_registry.cc"],
    hdrs = ["game_registry.h"],
    deps = [
        ":builtins",
        "//game_mcts/tournament_server/benchgame:bench_game",
        "//game_mcts/tournament_server/benchgame:bench_serialization",
        ":game_session",
        "//game_mcts/cpp/mcts:game_traits",
        "//game_mcts/cpp/mcts:mcts",
        "//game_mcts/cpp/risk:risk_game",
        "//game_mcts/cpp/risk:risk_serialization",
        "//game_mcts/cpp/risk/strategies:risk_proposer",
        "//game_mcts/cpp/risk/strategies:risk_rollout_shortcuts",
        "//game_mcts/cpp/tictactoe:tictactoe",
        "//game_mcts/cpp/tictactoe:tictactoe_serialization",
    ],
)

cc_library(
    name = "elo_store",
    srcs = ["elo_store.cc"],
    hdrs = ["elo_store.h"],
    deps = [
        ":tournament_broker_cc_proto",
        "@abseil-cpp//absl/log",
    ],
)

cc_library(
    name = "game_history",
    srcs = ["game_history.cc"],
    hdrs = ["game_history.h"],
    deps = [
        ":tournament_broker_cc_proto",
        "@abseil-cpp//absl/log",
    ],
)

cc_library(
    name = "client_handle",
    hdrs = ["client_handle.h"],
    deps = [":tournament_broker_cc_proto"],
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
        ":tournament_broker_cc_proto",
        ":worker_pool",
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
        ":tournament_broker_cc_proto",
        ":worker_pool",
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
        ":tournament_broker_cc_proto",
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
        ":tournament_broker_cc_grpc",
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
        ":tournament_broker_cc_grpc",
        "@grpc//:grpc++",
    ],
)

# Agent-facing arena RPCs, consumed by the MCP server.
cc_library(
    name = "arena_service",
    srcs = ["arena_service.cc"],
    hdrs = ["arena_service.h"],
    deps = [
        ":arena_cc_grpc",
        ":candidate_store",
        ":elo_store",
        ":scheduler",
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
        ":arena_cc_grpc",
        ":fleet_worker",
        ":scheduler",
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

cc_binary(
    name = "random_client",
    srcs = ["random_client.cc"],
    deps = [
        ":game_registry",
        ":tournament_broker_cc_grpc",
        "@abseil-cpp//absl/flags:flag",
        "@abseil-cpp//absl/flags:parse",
        "@abseil-cpp//absl/log",
        "@abseil-cpp//absl/log:initialize",
        "@grpc//:grpc++",
    ],
)

# Stock TournamentPolicy adapters (ProposerPolicy, MctsPolicy). Split from
# remote_client so a strategy can be defined without pulling in gRPC.
cc_library(
    name = "policies",
    hdrs = ["policies.h"],
    deps = [
        "//game_mcts/cpp/mcts:game_traits",
        "//game_mcts/cpp/mcts:mcts",
        "//game_mcts/cpp/mcts:tournament",
    ],
)

# Client-side strategy wrapper: PlayRemoteGames joins any TournamentPolicy<G>
# to a broker; ProposerPolicy and MctsPolicy come with it via policies.h.
cc_library(
    name = "remote_client",
    hdrs = ["remote_client.h"],
    deps = [
        ":policies",
        ":tournament_broker_cc_grpc",
        "//game_mcts/cpp/mcts:game_traits",
        "//game_mcts/cpp/mcts:mcts",
        "//game_mcts/cpp/mcts:serialization",
        "//game_mcts/cpp/mcts:tournament",
        "@abseil-cpp//absl/log",
        "@grpc//:grpc++",
    ],
)

cc_binary(
    name = "risk_mcts_client",
    srcs = ["risk_mcts_client.cc"],
    deps = [
        ":remote_client",
        ":tournament_broker_cc_grpc",
        "//game_mcts/cpp/risk:risk_game",
        "//game_mcts/cpp/risk:risk_serialization",
        "//game_mcts/cpp/risk/strategies:risk_proposer",
        "//game_mcts/cpp/risk/strategies:risk_rollout_shortcuts",
        "@abseil-cpp//absl/flags:flag",
        "@abseil-cpp//absl/flags:parse",
        "@abseil-cpp//absl/log",
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

# In-process load driver: move throughput vs concurrent clients. Compiles
# against the pre-refactor broker too, so the two concurrency models can be
# measured with the same harness.
cc_binary(
    name = "throughput_benchmark",
    srcs = ["throughput_benchmark.cc"],
    deps = [
        "//game_mcts/tournament_server/benchgame:bench_game",
        "//game_mcts/tournament_server/benchgame:bench_serialization",
        ":broker_service",
        ":elo_store",
        ":game_history",
        ":matchmaker",
        ":tournament_broker_cc_grpc",
        "@abseil-cpp//absl/flags:flag",
        "@abseil-cpp//absl/flags:parse",
        "@abseil-cpp//absl/log:globals",
        "@abseil-cpp//absl/log:initialize",
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
        "//game_mcts/cpp/tictactoe:tictactoe",
        "//game_mcts/cpp/tictactoe:tictactoe_cc_proto",
        "//game_mcts/cpp/tictactoe:tictactoe_serialization",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "game_session_test",
    srcs = ["game_session_test.cc"],
    deps = [
        ":game_registry",
        ":game_session",
        "//game_mcts/cpp/risk:risk_game",
        "//game_mcts/cpp/risk:risk_serialization",
        "//game_mcts/cpp/risk/strategies:risk_proposer",
        "//game_mcts/cpp/tictactoe:tictactoe",
        "//game_mcts/cpp/tictactoe:tictactoe_cc_proto",
        "//game_mcts/cpp/tictactoe:tictactoe_serialization",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "arena_integration_test",
    srcs = ["arena_integration_test.cc"],
    deps = [
        ":arena_cc_grpc",
        ":arena_service",
        ":candidate_store",
        ":elo_store",
        ":fleet_service",
        ":scheduler",
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
        ":remote_client",
        ":tournament_broker_cc_grpc",
        "//game_mcts/cpp/tictactoe:tictactoe",
        "//game_mcts/cpp/tictactoe:tictactoe_serialization",
        "@googletest//:gtest_main",
        "@grpc//:grpc++",
    ],
)
