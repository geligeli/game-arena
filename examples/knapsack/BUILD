load("@game_arena//game_arena/rules:problem.bzl", "arena_problem")

# The whole tournament, from one call: a config test, `bazel run //:tournament`,
# `//:kit` and `//:sandbox_image`, and `bazel build //:knapsack` for every
# binary a tournament needs. No registry: a graded problem has no referee. See
# @game_arena//game_arena/rules:problem.bzl.
arena_problem(
    name = "knapsack",
    config = "problem.textproto",
    # What a participant receives: the shape of a solution and this README.
    # Not the grader and not the cases -- a participant who can read the
    # cases can special-case them.
    kit_files = [
        "README.md",
        "//solutions/reference:kit",
    ],
)
