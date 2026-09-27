load("@grpc//bazel:cc_grpc_library.bzl", "cc_grpc_library")
load("@protobuf//bazel:cc_proto_library.bzl", "cc_proto_library")
load("@protobuf//bazel:proto_library.bzl", "proto_library")

package(default_visibility = ["//visibility:public"])

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
    generate_mocks = True,
    grpc_only = True,
    deps = [":tournament_broker_cc_proto"],
)

# Apart from problem.proto, which is published; this one is not.
proto_library(
    name = "clients_proto",
    srcs = ["clients.proto"],
    deps = [":problem_proto"],
)

cc_proto_library(
    name = "clients_cc_proto",
    deps = [":clients_proto"],
)

proto_library(
    name = "problem_proto",
    srcs = ["problem.proto"],
)

cc_proto_library(
    name = "problem_cc_proto",
    deps = [":problem_proto"],
)

# Handed to participants: no import, nothing about orders, sandboxes or scheduling.
proto_library(
    name = "kit_proto",
    srcs = ["kit.proto"],
)

cc_proto_library(
    name = "kit_cc_proto",
    deps = [":kit_proto"],
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

filegroup(
    name = "kit_surface_files",
    srcs = glob(["**"]),
)
