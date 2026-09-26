load("@grpc//bazel:cc_grpc_library.bzl", "cc_grpc_library")
load("@protobuf//bazel:cc_proto_library.bzl", "cc_proto_library")
load("@protobuf//bazel:proto_library.bzl", "proto_library")
load("@protobuf//bazel:py_proto_library.bzl", "py_proto_library")

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

# The kit's own config, as a participant edits it. Its own file because it is
# the one schema handed to participants: no import, and nothing about orders,
# sandboxes or scheduling in it.
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

# The sandbox job description is NOT here: it lives beside the engine that
# consumes it, at //game_arena/sandbox/exec:sandbox_job_proto, and the dev
# runner's service with the runner at
# //game_arena/sandbox/runner:sandbox_service_proto.
#
# The rule this package carries -- "the contract with every deployed worker
# and client" -- is true of arena.proto and not of a job description handed
# between two libraries in one process. Keeping it out also makes the
# engine's independence checkable by reading one BUILD file.

# Every file of this package, for the arena a participant's kit builds
# against: a kit gets these packages whole (see //:kit_surface).
filegroup(
    name = "kit_surface_files",
    srcs = glob(["**"]),
)
