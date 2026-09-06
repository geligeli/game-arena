load("@rules_cc//cc:cc_library.bzl", "cc_library")
load("@rules_cc//cc:cc_test.bzl", "cc_test")

package(
    default_visibility = ["//visibility:public"],
)

# The referee: the match loop and the broker protocol the two sides of a match
# speak. Everything here is game-agnostic -- it only ever sees serialized
# states and actions as byte strings.
#
# The rules themselves arrive through :game_registry, which is a DECLARATION
# with no definition in this repo. A referee binary is assembled downstream as
# "a registry + :referee_main"; see //game_arena/testgame for
# the reference assembly, and //game_mcts/arena in the game-mcts repo for a
# registry of real games. That is
# the seam that lets one arena host different problems with different referees
# compiled in.
#
# This package is deliberately NOT a dependency of
# //game_arena/server, which is a pure coordinator; that
# package's no_problem_code_test enforces it. Matches run here, in the sandbox,
# on a worker.

# The type-erased contract between the arena and a problem: implement
# GameSession, hand back a GameDescriptor. No dependency on any game framework.
cc_library(
    name = "game_session",
    srcs = ["game_session.cc"],
    hdrs = ["game_session.h"],
)

# The registry interface: name -> GameDescriptor. Header only, on purpose --
# GameRegistry() is defined by whichever registry the final binary links.
cc_library(
    name = "game_registry",
    hdrs = ["game_registry.h"],
    deps = [":game_session"],
)

cc_library(
    name = "client_handle",
    hdrs = ["client_handle.h"],
    deps = ["//game_arena/proto:tournament_broker_cc_proto"],
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
        ":game_session",
        ":worker_pool",
        "//game_arena/proto:tournament_broker_cc_proto",
        "//game_arena/server:elo_store",
        "//game_arena/server:game_history",
        "@abseil-cpp//absl/log",
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
        "//game_arena/server:elo_store",
        "//game_arena/server:game_history",
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
        "//game_arena/proto:tournament_broker_cc_proto",
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
        "//game_arena/proto:tournament_broker_cc_grpc",
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
        "//game_arena/proto:tournament_broker_cc_grpc",
        "@grpc//:grpc++",
    ],
)

# One match, then exit. Started by a sandbox worker per order; see main.cc.
#
# A library rather than a cc_binary because main() is game-agnostic but the
# registry it drives is not: link this with a registry to get a referee.
# alwayslink so main() survives the archive.
cc_library(
    name = "referee_main",
    srcs = ["main.cc"],
    alwayslink = 1,
    deps = [
        ":broker_service",
        ":game_registry",
        ":matchmaker",
        "//game_arena/proto:tournament_broker_cc_proto",
        "//game_arena/server:elo_store",
        "//game_arena/server:game_history",
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
# arena's evaluation path -- that is :referee_main, one match at a time.
# Same registry-linking contract as :referee_main.
cc_library(
    name = "broker_server_main",
    srcs = ["broker_main.cc"],
    alwayslink = 1,
    deps = [
        ":broker_service",
        ":game_registry",
        ":matchmaker",
        "//game_arena/server:elo_standings",
        "//game_arena/server:elo_store",
        "//game_arena/server:game_history",
        "//game_arena/server:http_leaderboard",
        "@abseil-cpp//absl/flags:flag",
        "@abseil-cpp//absl/flags:parse",
        "@abseil-cpp//absl/log",
        "@abseil-cpp//absl/log:globals",
        "@abseil-cpp//absl/log:initialize",
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
