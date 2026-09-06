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
        "//game_mcts/tournament_server/server:client_registry",
        "@abseil-cpp//absl/flags:flag",
        "@abseil-cpp//absl/flags:parse",
        "@abseil-cpp//absl/log:globals",
        "@abseil-cpp//absl/log:initialize",
    ],
)
