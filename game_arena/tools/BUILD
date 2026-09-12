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
