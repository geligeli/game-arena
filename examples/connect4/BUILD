load("@game_arena//game_arena/rules:problem.bzl", "arena_problem")

# The whole tournament, from one call: the referee and a local broker linked
# from //game:registry, a config test, `bazel run //:tournament`, `//:kit` and
# `//:sandbox_image`, and `bazel build //:connect4` for every binary a
# tournament needs. See @game_arena//game_arena/rules:problem.bzl.
arena_problem(
    name = "connect4",
    config = "problem.textproto",
    registry = "//game:registry",
    # What a participant receives: the rules (so a bot can search them), the
    # harness and the reference bot. Not the config, not the tournament.
    kit_files = [
        "//bots:kit",
        "//game:kit",
    ],
)
