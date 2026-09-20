load("@rules_cc//cc:cc_binary.bzl", "cc_binary")

package(default_visibility = ["//visibility:public"])

# A submission is exactly this: one cc_binary named "solve". The problem builds
# //solutions/<submission_id>:solve and hands the grader its path.
cc_binary(
    name = "solve",
    srcs = ["solve.cc"],
)

# What a participant receives, for the kit `bazel run //:kit` writes.
filegroup(
    name = "kit",
    srcs = [
        "BUILD",
        "solve.cc",
    ],
)

# Everything here, for the sandbox image: a glob does not cross packages, so
# each one exports its own (arena_problem's `tree`).
filegroup(
    name = "tree",
    srcs = glob(["**"]),
    visibility = ["//visibility:public"],
)
