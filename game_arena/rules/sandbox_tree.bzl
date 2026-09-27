"""Source files as a tar layer, each at its path in its own repository under `prefix`."""

def _sandbox_tree_impl(ctx):
    out = ctx.actions.declare_file(ctx.label.name + ".tar")
    files = sorted(ctx.files.srcs, key = lambda f: f.path)
    listing = ctx.actions.declare_file(ctx.label.name + ".files")
    ctx.actions.write(listing, "".join([f.path + "\n" for f in files]))
    ctx.actions.run_shell(
        outputs = [out],
        inputs = files + [listing],
        # Readable by anyone: the worker hands the copy to whoever the sandbox runs as.
        command = "tar --create --file \"$2\" --dereference --mtime=@0 " +
                  "--owner=0 --group=0 --numeric-owner --mode=a+rX " +
                  "--transform='s,^external/[^/]*/,,S' " +
                  "--transform=\"s,^,$3/,S\" --files-from=\"$1\"",
        arguments = [listing.path, out.path, ctx.attr.prefix],
        use_default_shell_env = True,
        mnemonic = "ArenaSandboxTree",
        progress_message = "Archiving %{label}",
    )
    return [DefaultInfo(files = depset([out]))]

sandbox_tree = rule(
    implementation = _sandbox_tree_impl,
    attrs = {
        "srcs": attr.label_list(allow_files = True),
        "prefix": attr.string(mandatory = True),
    },
)
