load("@game_arena//game_arena/rules:problem.bzl", "arena_problem")

# The whole tournament: @game_arena//game_arena/rules:problem.bzl lists the targets.
arena_problem(
    name = "knapsack",
    config = "problem.textproto",
    # Not the grader or the cases: a participant who can read the cases can special-case them.
    kit_files = [
        "README.md",
        "//solutions/reference:kit",
    ],
    tree = [
        "//grader:tree",
        "//solutions/greedy:tree",
        "//solutions/reference:tree",
    ],
)
