package(
    default_visibility = ["//visibility:public"],
)

# The problem definitions this repo ships. One server runs one of these.
# Kept under test by //game_mcts/tournament_server/server:shipped_problems_test,
# so a config that stops parsing fails the build rather than a deployment.
filegroup(
    name = "configs",
    srcs = glob(["*.textproto"]),
)
