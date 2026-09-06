load("@grpc//bazel:cc_grpc_library.bzl", "cc_grpc_library")
load("@protobuf//bazel:cc_proto_library.bzl", "cc_proto_library")
load("@protobuf//bazel:proto_library.bzl", "proto_library")

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

proto_library(
    name = "sandbox_runner_proto",
    srcs = ["sandbox_runner.proto"],
)

cc_proto_library(
    name = "sandbox_runner_cc_proto",
    deps = [":sandbox_runner_proto"],
)

cc_grpc_library(
    name = "sandbox_runner_cc_grpc",
    srcs = [":sandbox_runner_proto"],
    grpc_only = True,
    deps = [":sandbox_runner_cc_proto"],
)
