package(
    default_visibility = ["//visibility:public"],
)

# The arena's reference problem, on the arena's own game. One server runs one
# problem; the game_mcts problems live in //game_mcts/arena/problems.
# Kept under test by //game_mcts/tournament_server/server:shipped_problems_test,
# so a config that stops parsing fails the build rather than a deployment.
filegroup(
    name = "configs",
    srcs = glob(["*.textproto"]),
)
