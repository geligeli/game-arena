load("@rules_cc//cc:cc_library.bzl", "cc_library")

package(default_visibility = ["//visibility:public"])

# A bot for any problem is this plus one function; see examples/.
cc_library(
    name = "play_loop",
    srcs = ["play_loop.cc"],
    hdrs = ["play_loop.h"],
    deps = [
        "//game_arena/proto:tournament_broker_cc_grpc",
        "@abseil-cpp//absl/log",
        "@grpc//:grpc++",
    ],
)

# A kit vendors this package whole (//:kit_surface).
filegroup(
    name = "kit_surface_files",
    srcs = glob(["**"]),
)
