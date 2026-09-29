load("@game_arena//game_arena/rules:problem.bzl", "arena_problem")

exports_files(["problem.textproto"])

# The whole tournament: @game_arena//game_arena/rules:problem.bzl lists the targets.
arena_problem(
    name = "connect4",
    config = "problem.textproto",
    registry = "//game:registry",
    kit_base = "@kit_agents",
    # The rules (so a bot can search them), the harness and the reference bot.
    kit_files = [
        "//bots:kit",
        "//bots/reference:kit",
        "//game:kit",
    ],
    tree = [
        "//bots:tree",
        "//game:tree",
    ],
)
