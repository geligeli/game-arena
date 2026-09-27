load("@rules_cc//cc:cc_binary.bzl", "cc_binary")

package(default_visibility = ["//visibility:public"])

cc_binary(
    name = "solve",
    srcs = ["solve.cc"],
)

# A glob does not cross packages, so each exports its own (arena_problem's `tree`).
filegroup(
    name = "tree",
    srcs = glob(["**"]),
)
