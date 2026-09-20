package(default_visibility = ["//visibility:public"])

# The one macro a problem repository calls: problem.bzl's arena_problem. The
# script is what its generated sh_binary/sh_test targets run; it only finds the
# arena_tournament tool in runfiles and execs it. kit_tree.bzl and
# tournament_tree.bzl are the rules the macro's images are built from.
exports_files([
    "kit_tree.bzl",
    "problem.bzl",
    "run_tool.sh",
    "tournament_tree.bzl",
])
