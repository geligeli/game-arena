load("@rules_cc//cc:cc_binary.bzl", "cc_binary")

package(default_visibility = ["//visibility:public"])

# The scoring binary. It is part of the problem, not of a submission, and
# problem.textproto denies patches that touch it -- editing the grader is
# editing the scoreboard.
cc_binary(
    name = "grade",
    srcs = ["grade_main.cc"],
    deps = [
        "@abseil-cpp//absl/flags:flag",
        "@abseil-cpp//absl/flags:parse",
        # Runs the submission with a timeout and an address-space cap.
        "@game_arena//game_arena/common/process",
        # Writes the JSON the worker scores from.
        "@game_arena//game_arena/grader:report",
    ],
)
