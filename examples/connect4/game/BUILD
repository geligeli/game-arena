load("@rules_cc//cc:cc_binary.bzl", "cc_binary")
load("@rules_cc//cc:cc_library.bzl", "cc_library")
load("@rules_cc//cc:cc_test.bzl", "cc_test")

package(default_visibility = ["//visibility:public"])

# The rules. Depends on one arena header -- the GameSession contract -- and
# nothing else.
cc_library(
    name = "connect4",
    srcs = ["connect4.cc"],
    hdrs = ["connect4.h"],
    deps = ["@game_arena//game_arena/referee:game_session"],
)

# The definition of GameRegistry(). alwayslink because nothing depends on it by
# label: it exists to satisfy a symbol the arena leaves undefined.
cc_library(
    name = "registry",
    srcs = ["registry.cc"],
    alwayslink = 1,
    deps = [
        ":connect4",
        "@game_arena//game_arena/referee:game_registry",
    ],
)

# A referee for this problem: our rules, the arena's match loop. This two-line
# rule is the whole of what "run my game in the arena" means.
cc_binary(
    name = "match_referee",
    deps = [
        ":registry",
        "@game_arena//game_arena/referee:referee_main",
    ],
)

# A long-running broker for local play, and the generic random client pointed
# at our registry. Neither is on the evaluation path; both are for iterating.
cc_binary(
    name = "broker_server",
    deps = [
        ":registry",
        "@game_arena//game_arena/referee:broker_server_main",
    ],
)

cc_binary(
    name = "random_client",
    deps = [
        ":registry",
        "@game_arena//game_arena/client:random_client_main",
    ],
)

cc_test(
    name = "connect4_test",
    srcs = ["connect4_test.cc"],
    deps = [
        ":connect4",
        ":registry",
        "@game_arena//game_arena/referee:game_registry",
        "@googletest//:gtest_main",
    ],
)
