load("@rules_cc//cc:cc_binary.bzl", "cc_binary")
load("@rules_cc//cc:cc_library.bzl", "cc_library")
load("@rules_cc//cc:cc_test.bzl", "cc_test")

package(
    default_visibility = ["//visibility:public"],
)

# The arena's own game, and the reference assembly of a referee.
#
# Nim is here so the broker core can be tested end to end with no game
# framework in the build. It is also worked example: :game_registry is the
# entire obligation a downstream repo takes on to host its own problems, and
# the binaries below show the "registry + entry-point library" pattern.

cc_library(
    name = "nim",
    srcs = ["nim.cc"],
    hdrs = ["nim.h"],
    deps = [
        "//game_mcts/tournament_server/referee:game_session",
        "@abseil-cpp//absl/log:check",
    ],
)

# The definition of GameRegistry() that //game_mcts/tournament_server/referee
# only declares. Linking this into a binary decides which games it can run.
# alwayslink because nothing depends on it by label -- it exists to satisfy an
# undefined symbol, and the linker would otherwise skip the archive member.
cc_library(
    name = "game_registry",
    srcs = ["game_registry.cc"],
    alwayslink = 1,
    deps = [
        ":nim",
        "//game_mcts/tournament_server/referee:game_registry",
    ],
)

# A referee that knows one game. Same two-line shape as //game_mcts/arena's.
cc_binary(
    name = "match_referee",
    deps = [
        ":game_registry",
        "//game_mcts/tournament_server/referee:referee_main",
    ],
)

cc_binary(
    name = "broker_server",
    deps = [
        ":game_registry",
        "//game_mcts/tournament_server/referee:broker_server_main",
    ],
)

cc_binary(
    name = "random_client",
    deps = [
        ":game_registry",
        "//game_mcts/tournament_server/client:random_client_main",
    ],
)

cc_test(
    name = "nim_test",
    srcs = ["nim_test.cc"],
    deps = [
        ":game_registry",
        ":nim",
        "//game_mcts/tournament_server/referee:game_registry",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "nim_game_run_test",
    srcs = ["nim_game_run_test.cc"],
    deps = [
        ":game_registry",
        ":nim",
        "//game_mcts/tournament_server/referee:client_handle",
        "//game_mcts/tournament_server/referee:game_registry",
        "//game_mcts/tournament_server/referee:game_run",
        "//game_mcts/tournament_server/referee:worker_pool",
        "//game_mcts/tournament_server/server:elo_store",
        "//game_mcts/tournament_server/server:game_history",
        "@googletest//:gtest_main",
    ],
)
