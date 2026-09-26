load("@rules_cc//cc:cc_binary.bzl", "cc_binary")

package(default_visibility = ["//visibility:public"])

# The participant's client, and the one the kit installs as a builtin.
#
# Its own package rather than one of //game_arena/tools, which is operator
# tooling: this is the only one of the three a participant runs, it is part of
# the arena a kit vendors (//:kit_surface), and the binary installed into a
# kit is the one that goes into an image of it: the toolchain is hermetic and
# links libc++ statically, so it needs a glibc and nothing else of its host.
#
# It talks the Arena service and reads the kit's arena.textproto. Nothing
# else: no problem config, no scheduler, no candidate store. What it can do is
# what a participant can do.
cc_binary(
    name = "arena_cli",
    srcs = ["arena_cli.cc"],
    deps = [
        "//game_arena/proto:arena_cc_grpc",
        "//game_arena/proto:kit_cc_proto",
        "//game_arena/proto:tournament_broker_cc_proto",
        "//game_arena/referee:match_tally",
        "@abseil-cpp//absl/flags:flag",
        "@abseil-cpp//absl/flags:parse",
        "@abseil-cpp//absl/log:globals",
        "@abseil-cpp//absl/log:initialize",
        "@grpc//:grpc++",
        "@protobuf//:protobuf",
    ],
)

# Every file of this package, for the arena a participant's kit builds
# against: a kit gets these packages whole (see //:kit_surface).
filegroup(
    name = "kit_surface_files",
    srcs = glob(["**"]),
)
