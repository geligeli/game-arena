load("@rules_cc//cc:cc_binary.bzl", "cc_binary")

package(default_visibility = ["//visibility:public"])

# Part of the problem, not a submission: allow_paths keeps patches out of it.
cc_binary(
    name = "grade",
    srcs = ["grade.cc"],
    deps = [
        "@abseil-cpp//absl/flags:flag",
        "@abseil-cpp//absl/flags:parse",
        "@game_arena//game_arena/common/process",
        "@game_arena//game_arena/grader:report",
    ],
)

# A glob does not cross packages, so each exports its own (arena_problem's `tree`).
filegroup(
    name = "tree",
    srcs = glob(["**"]),
)
