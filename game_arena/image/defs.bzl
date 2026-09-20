"""The regctl rules_oci builds images with, as a file a program can be handed.

rules_oci resolves regctl as a toolchain, which a rule can use and a binary's
`data` cannot name. `arena_tournament kit --image` adds a primed cache and a
participant's token to a built kit image at run time -- neither can be a build
input -- so it needs the same regctl as a runfile.
"""

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
