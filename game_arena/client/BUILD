load("@rules_cc//cc:cc_library.bzl", "cc_library")

package(
    default_visibility = ["//visibility:public"],
)

# The client half of the Play stream, with the game left out. A bot for any
# problem is this plus one function; see examples/.
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

# Every file of this package, for the arena a participant's kit builds
# against: a kit gets these packages whole (see //:kit_surface).
filegroup(
    name = "kit_surface_files",
    srcs = glob(["**"]),
)
