load("@rules_cc//cc:cc_binary.bzl", "cc_binary")
load("@rules_cc//cc:cc_library.bzl", "cc_library")
load("@rules_cc//cc:cc_test.bzl", "cc_test")

package(default_visibility = ["//visibility:public"])

# In //:kit_surface: no deps on sandbox/, server/, tools/ or common/process.
cc_binary(
    name = "arena_cli",
    srcs = ["arena_cli.cc"],
    deps = [
        ":mcp",
        "//game_arena/proto:arena_cc_grpc",
        "//game_arena/proto:kit_cc_proto",
        "//game_arena/proto:tournament_broker_cc_proto",
        "//game_arena/referee:match_tally",
        "@abseil-cpp//absl/flags:flag",
        "@abseil-cpp//absl/flags:parse",
        "@abseil-cpp//absl/log:globals",
        "@abseil-cpp//absl/log:initialize",
        "@abseil-cpp//absl/strings",
        "@grpc//:grpc++",
        "@protobuf//:protobuf",
    ],
)

cc_library(
    name = "mcp",
    srcs = ["mcp.cc"],
    hdrs = ["mcp.h"],
    deps = ["@boost.json"],
)

cc_test(
    name = "mcp_test",
    srcs = ["mcp_test.cc"],
    deps = [
        ":mcp",
        "@googletest//:gtest_main",
    ],
)

filegroup(
    name = "kit_surface_files",
    srcs = glob(["**"]),
)
