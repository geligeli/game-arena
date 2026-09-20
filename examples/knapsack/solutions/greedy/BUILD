load("@rules_cc//cc:cc_binary.bzl", "cc_binary")

package(default_visibility = ["//visibility:public"])

# The greedy submission. Same shape as any other: one cc_binary named "solve".

cc_binary(
    name = "solve",
    srcs = ["solve.cc"],
)

# Everything here, for the sandbox image: a glob does not cross packages, so
# each one exports its own (arena_problem's `tree`).
filegroup(
    name = "tree",
    srcs = glob(["**"]),
    visibility = ["//visibility:public"],
)
