# Submissions land here, one directory each, with a BUILD the coordinator
# generates. This file exists so the directory survives in git while empty.

# Everything here, for the sandbox image: a glob does not cross packages, so
# each one exports its own (arena_problem's `tree`).
filegroup(
    name = "tree",
    srcs = glob(["**"]),
    visibility = ["//visibility:public"],
)
