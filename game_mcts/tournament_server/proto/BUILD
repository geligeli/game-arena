load("@grpc//bazel:cc_grpc_library.bzl", "cc_grpc_library")
load("@protobuf//bazel:cc_proto_library.bzl", "cc_proto_library")
load("@protobuf//bazel:proto_library.bzl", "proto_library")
load("@protobuf//bazel:py_proto_library.bzl", "py_proto_library")
load("@rules_python//python:defs.bzl", "py_library")

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

py_proto_library(
    name = "arena_py_proto",
    deps = [":arena_proto"],
)

# Hermetic replacement for mcp_servers/arena_mcp/make_stubs.sh: the grpc
# python plugin is not part of protoc, so it runs as an explicit genrule.
# -I.../proto keeps the proto import path flat ("arena.proto"), matching
# py_proto_library's flat arena_pb2.py and the stub's "import arena_pb2".
genrule(
    name = "arena_grpc_py_gen",
    srcs = ["arena.proto"],
    outs = ["arena_pb2_grpc.py"],
    cmd = "$(location @protobuf//:protoc) " +
          "--plugin=protoc-gen-grpc_python=$(location @grpc//src/compiler:grpc_python_plugin) " +
          "--grpc_python_out=$(@D) -Igame_mcts/tournament_server/proto $<",
    tools = [
        "@grpc//src/compiler:grpc_python_plugin",
        "@protobuf//:protoc",
    ],
)

# Import surface for python consumers: "import arena_pb2[_grpc]" (flat).
py_library(
    name = "arena_py",
    srcs = [":arena_grpc_py_gen"],
    imports = ["."],
    deps = [":arena_py_proto"],
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
