package(default_visibility = ["//visibility:public"])

# The one macro a problem repository calls: problem.bzl's arena_problem. The
# script is what its generated sh_binary/sh_test targets run; it only finds the
# arena_tournament tool in runfiles and execs it. kit_tree.bzl and
# sandbox_tree.bzl are the rules the macro's images are built from.
exports_files([
    "kit_tree.bzl",
    "problem.bzl",
    "run_tool.sh",
    "sandbox_tree.bzl",
])

# What a sandbox needs of this package to load a problem's root BUILD.
filegroup(
    name = "sandbox_surface_files",
    srcs = glob(["**"]),
)
