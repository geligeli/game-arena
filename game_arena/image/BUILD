load("@rules_oci//oci:defs.bzl", "oci_image")
load(":defs.bzl", "regctl")

package(default_visibility = ["//visibility:public"])

exports_files(["sandbox.bazelrc"])

# manual, all of it: building any of this pulls the base, and `//...` must not need a registry.

# The root fs is read-only, so the worker unpacks bazel into its output-base volume;
# the user stays root because the worker chowns the tree to sandbox.run_as_user.
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

# uid 1000 ("ubuntu") owns /kit; a replacement kit_base has to keep that user.
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

genrule(
    name = "kit_entrypoint_layer",
    srcs = ["kit_entrypoint.sh"],
    outs = ["kit_entrypoint_layer.tar"],
    cmd = """
mkdir -p $(@D)/kit_entrypoint/usr/local/bin
cp $< $(@D)/kit_entrypoint/usr/local/bin/kit_entrypoint
tar --sort=name --mtime=@0 --owner=0 --group=0 --numeric-owner --mode=0755 \
    --create --file $@ --directory $(@D)/kit_entrypoint usr/local/bin/kit_entrypoint
""",
    tags = ["manual"],
)

regctl(
    name = "regctl",
    tags = ["manual"],
)
