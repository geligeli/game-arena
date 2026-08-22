load("@protobuf//bazel:cc_proto_library.bzl", "cc_proto_library")
load("@protobuf//bazel:proto_library.bzl", "proto_library")
load("@rules_cc//cc:cc_library.bzl", "cc_library")

package(
    default_visibility = ["//visibility:public"],
)

proto_library(
    name = "bench_proto",
    srcs = ["bench.proto"],
)

cc_proto_library(
    name = "bench_cc_proto",
    deps = [":bench_proto"],
)

# A no-op game used to load test the broker: every action is legal, so a client
# can replay one pre-serialized action and neither side spends time on rules.
cc_library(
    name = "bench_game",
    hdrs = ["bench_game.h"],
    deps = ["@game_mcts//game_mcts/cpp/mcts:game_traits"],
)

cc_library(
    name = "bench_serialization",
    hdrs = ["bench_serialization.h"],
    deps = [
        ":bench_cc_proto",
        ":bench_game",
        "@game_mcts//game_mcts/cpp/mcts:serialization",
    ],
)
