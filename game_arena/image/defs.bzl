"""rules_oci's regctl toolchain as a runfile, which a binary's `data` can name."""

_REGCTL = "@rules_oci//oci:regctl_toolchain_type"

def _regctl_impl(ctx):
    regctl = ctx.toolchains[_REGCTL].regctl_info.binary
    return [DefaultInfo(
        files = depset([regctl]),
        runfiles = ctx.runfiles(files = [regctl]),
    )]

regctl = rule(
    implementation = _regctl_impl,
    toolchains = [_REGCTL],
)
