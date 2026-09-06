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

# The reference client: connects to a broker, asks the registry what the game's
# legal moves look like, and plays uniformly at random. Game-agnostic -- it
# only ever handles serialized states and actions.
#
# A library holding main(), not a cc_binary, for the same reason as
# //game_arena/referee:referee_main: link it with a registry
# to get a client that can play that registry's games.
cc_library(
    name = "random_client_main",
    srcs = ["random_client.cc"],
    alwayslink = 1,
    deps = [
        ":play_loop",
        "//game_arena/proto:tournament_broker_cc_grpc",
        "//game_arena/referee:game_registry",
        "@abseil-cpp//absl/flags:flag",
        "@abseil-cpp//absl/flags:parse",
        "@abseil-cpp//absl/log",
        "@abseil-cpp//absl/log:initialize",
        "@grpc//:grpc++",
    ],
)
