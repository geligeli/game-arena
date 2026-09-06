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

# Who may submit, and how much. Kept apart from problem.proto because that one
# is published and this one is not.
proto_library(
    name = "clients_proto",
    srcs = ["clients.proto"],
    deps = [":problem_proto"],
)

cc_proto_library(
    name = "clients_cc_proto",
    deps = [":clients_proto"],
)

# What a problem is, as data: repo, submission policy, build, sandbox, how a
# solution is scored, and per-client limits. Embedded in every work order, so
# workers carry no problem config of their own.
proto_library(
    name = "problem_proto",
    srcs = ["problem.proto"],
)

cc_proto_library(
    name = "problem_cc_proto",
    deps = [":problem_proto"],
)

py_proto_library(
    name = "problem_py_proto",
    deps = [":problem_proto"],
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
          "--grpc_python_out=$(@D) -Igame_arena/proto $<",
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
