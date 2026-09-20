load("@rules_oci//oci:defs.bzl", "oci_image")
load(":defs.bzl", "regctl")

package(default_visibility = ["//visibility:public"])

# The arena's images, as far as the arena can build them: the base
# (docker/base/Dockerfile, pulled by digest -- see MODULE.bazel) with what is
# true of any kit, or of any sandbox, set on it. A problem's own images are
# arena_problem()'s to build, on these.
#
# manual, all of it: building any of this pulls the base from its registry, and
# `//...` -- a consumer's, a kit's, a sandbox's `bazel vendor` -- must not need
# one.

# The base itself, for an image that adds to it without changing what it is.
alias(
    name = "base",
    actual = "@arena_base",
    tags = ["manual"],
)

# What every sandbox image starts from: the base, and a system bazelrc that
# builds from /opt/arena/vendor. What goes *in* that directory is every
# external repository one problem resolves, which is the result of running
# `bazel vendor` on it -- not a build's to produce, so `arena_tournament image`
# adds it to this as a layer, outside one.
#
# Bazel's own installation is not unpacked here: it wants a lock file beside
# itself, and a sandbox's root filesystem is read-only, so the worker points
# --output_user_root into the persistent output-base volume instead and pays
# for the unpack once per slot and per bazel. The default user stays root: who a sandbox runs as
# is the problem's choice (sandbox.run_as_user), and the tree it is handed is
# chowned to that user by the worker before anything starts.
genrule(
    name = "sandbox_rc_layer",
    srcs = ["sandbox.bazelrc"],
    outs = ["sandbox_rc_layer.tar"],
    cmd = """
mkdir -p $(@D)/sandbox_rc/etc
cp $< $(@D)/sandbox_rc/etc/bazel.bazelrc
tar --sort=name --mtime=@0 --owner=0 --group=0 --numeric-owner --mode=0644 \
    --create --file $@ --directory $(@D)/sandbox_rc etc/bazel.bazelrc
""",
    tags = ["manual"],
)

oci_image(
    name = "sandbox_base",
    base = "@arena_base",
    tags = ["manual"],
    tars = [":sandbox_rc_layer"],
    workdir = "/",
)

# What every kit image starts from: the arena's base (docker/base/Dockerfile,
# pulled by digest -- see MODULE.bazel) with what is true of any kit set on
# it. Nothing of a problem and nothing of a participant: arena_problem()'s
# kit_image stacks a problem's kit on this, as a build output, and its
# kit_image_issue adds a primed cache and a participant's token to that
# outside the build -- a minted token must never be the input of an action,
# where a remote cache would keep it.
#
# uid 1000 ("ubuntu" in the base) owns /kit; a base that replaces this one
# (arena_problem's kit_base) has to keep that user.
#
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

# What adds to a built kit image at run time, and pushes it. See defs.bzl.
regctl(
    name = "regctl",
    tags = ["manual"],
)
