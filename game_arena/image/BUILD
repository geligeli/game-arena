load("@rules_oci//oci:defs.bzl", "oci_image")
load(":defs.bzl", "regctl")

package(default_visibility = ["//visibility:public"])

# The Dockerfile arena_tournament builds a problem's sandbox and tournament
# images from. Data of the tool, not built here: each of those runs bazel
# inside the image it is making, which is the one thing a layered image cannot
# do.
exports_files(["Dockerfile"])

# What every kit image starts from: the arena's base (docker/base/Dockerfile,
# pulled by digest -- see MODULE.bazel) with what is true of any kit set on
# it. Nothing of a problem and nothing of a participant: the kit itself is a
# layer `arena_tournament kit --image` adds to this at run time, because a
# minted token must never be the input of a build action, where a remote cache
# would keep it.
#
# uid 1000 ("ubuntu" in the base) owns /kit; a base that replaces this one
# (arena_problem's kit_base) has to keep that user.
#
# manual: building this pulls the base from its registry, and `//...` -- a
# consumer's, a kit's, a sandbox's `bazel vendor` -- must not need one.
oci_image(
    name = "kit_base",
    base = "@arena_base",
    env = {
        "ARENA_KIT": "/kit",
        "PATH": "/kit/.arena/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin",
    },
    tags = ["manual"],
    user = "ubuntu",
    workdir = "/kit",
)

# What layers a kit onto :kit_base at run time, and pushes it. See defs.bzl.
regctl(
    name = "regctl",
    tags = ["manual"],
)
