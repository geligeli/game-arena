load("@rules_cc//cc:cc_binary.bzl", "cc_binary")

package(
    default_visibility = ["//visibility:public"],
)

# Operator tooling for the arena. Not part of the server: nothing here runs in
# the request path.
cc_binary(
    name = "arena_admin",
    srcs = ["arena_admin.cc"],
    deps = [
        "//game_arena/server:client_registry",
        "@abseil-cpp//absl/flags:flag",
        "@abseil-cpp//absl/flags:parse",
        "@abseil-cpp//absl/log:globals",
        "@abseil-cpp//absl/log:initialize",
    ],
)

# A human's client for the Arena service: submit a candidate, poll its job,
# read standings and rivals' source. Same RPCs as the MCP server, driven from
# a shell instead of an agent.
cc_binary(
    name = "arena_cli",
    srcs = ["arena_cli.cc"],
    deps = [
        "//game_arena/proto:arena_cc_grpc",
        "@abseil-cpp//absl/flags:flag",
        "@abseil-cpp//absl/flags:parse",
        "@abseil-cpp//absl/log:globals",
        "@abseil-cpp//absl/log:initialize",
        "@grpc//:grpc++",
    ],
)

# Standing a tournament up from a problem repository, and handing out kits to
# enter it. Driven through the targets @game_arena//game_arena/rules:problem.bzl
# generates; the arena binaries it launches ride along as data.
cc_binary(
    name = "arena_tournament",
    srcs = ["arena_tournament.cc"],
    data = [
        "//game_arena/sandbox/image:Dockerfile",
        "//game_arena/sandbox/worker:sandbox_worker",
        "//game_arena/server:problem_server",
    ],
    deps = [
        "//game_arena/common/process",
        "//game_arena/server:client_registry",
        "//game_arena/server:problem_config",
        "@abseil-cpp//absl/flags:flag",
        "@abseil-cpp//absl/flags:parse",
        "@abseil-cpp//absl/log",
        "@abseil-cpp//absl/log:globals",
        "@abseil-cpp//absl/log:initialize",
        "@abseil-cpp//absl/strings",
        "@protobuf//:protobuf",
        "@rules_cc//cc/runfiles",
    ],
)
