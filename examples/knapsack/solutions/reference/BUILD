load("@rules_cc//cc:cc_binary.bzl", "cc_binary")

package(default_visibility = ["//visibility:public"])

# A submission is exactly this: one cc_binary named "solve".
cc_binary(
    name = "solve",
    srcs = ["solve.cc"],
)

filegroup(
    name = "kit",
    srcs = [
        "BUILD",
        "solve.cc",
    ],
)

# A glob does not cross packages, so each exports its own (arena_problem's `tree`).
filegroup(
    name = "tree",
    srcs = glob(["**"]),
)
