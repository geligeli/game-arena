load("@rules_cc//cc:cc_library.bzl", "cc_library")
load("@rules_cc//cc:cc_test.bzl", "cc_test")

package(default_visibility = ["//visibility:public"])

# A bot for any problem is this plus one function; see examples/.
cc_library(
    name = "play_loop",
    srcs = ["play_loop.cc"],
    hdrs = ["play_loop.h"],
    deps = [
        "//game_arena/proto:tournament_broker_cc_grpc",
        "@abseil-cpp//absl/log",
        "@abseil-cpp//absl/strings",
        "@grpc//:grpc++",
    ],
)

cc_test(
    name = "play_loop_test",
    srcs = ["play_loop_test.cc"],
    deps = [
        ":play_loop",
        "//game_arena/proto:tournament_broker_cc_grpc",
        "@googletest//:gtest_main",
        "@grpc//:grpc++_test",
    ],
)

# A kit vendors this package whole (//:kit_surface).
filegroup(
    name = "kit_surface_files",
    srcs = glob(["**"]),
)
