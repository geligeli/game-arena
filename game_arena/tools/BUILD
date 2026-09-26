load("@rules_cc//cc:cc_binary.bzl", "cc_binary")
load("@rules_cc//cc:cc_library.bzl", "cc_library")
load("@rules_cc//cc:cc_test.bzl", "cc_test")

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
        "@abseil-cpp//absl/flags:usage",
        "@abseil-cpp//absl/log:globals",
        "@abseil-cpp//absl/log:initialize",
    ],
)

# Standing a tournament up from a problem repository, and handing out kits to
# enter it. Driven through the targets @game_arena//game_arena/rules:problem.bzl
# generates; the arena binaries it launches ride along as data.
cc_binary(
    name = "arena_tournament",
    srcs = ["arena_tournament.cc"],
    data = [
        "//game_arena/sandbox/worker:sandbox_worker",
        "//game_arena/server:problem_server",
        # Installed into every kit as a builtin, not run from here.
        "//game_arena/cli:arena_cli",
        # Copied into every kit as the arena it builds against.
        "//:kit_surface",
        # The rc a sandbox reads, which priming one on this host follows.
        "//game_arena/image:sandbox.bazelrc",
    ],
    deps = [
        ":sandbox_priming",
        "//game_arena/common/kv_options",
        "//game_arena/common/process",
        "//game_arena/proto:kit_cc_proto",
        "//game_arena/sandbox/common:docker",
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
        "@boost.json",
    ],
)

# The sandbox's own targets, rc and owners, for a priming build on the host.
cc_library(
    name = "sandbox_priming",
    srcs = ["sandbox_priming.cc"],
    hdrs = ["sandbox_priming.h"],
    deps = [
        "//game_arena/proto:problem_cc_proto",
        "//game_arena/server:problem_config",
        "@abseil-cpp//absl/strings",
    ],
)

cc_test(
    name = "sandbox_priming_test",
    srcs = ["sandbox_priming_test.cc"],
    data = ["//game_arena/image:sandbox.bazelrc"],
    deps = [
        ":sandbox_priming",
        "@googletest//:gtest_main",
    ],
)
