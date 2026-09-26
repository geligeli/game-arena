"""A participant's kit as a tar of /kit: the layer a kit image is made of.

`arena_tournament kit` writes a kit wherever it is told; this runs it in a
build action and archives the result, so that the image of a kit is a build
output like any other:

    bazel build //:kit_image

What is in it is what `bazel run //:kit` writes, less what cannot be a build's
business: no token, since an action's inputs end up in a remote cache, and
nothing primed, since that is bazel run on the kit. Priming is added
afterwards, outside the build (the macro's kit_image_issue target); a token
never is.
"""

def _kit_tree_impl(ctx):
    out = ctx.actions.declare_file(ctx.label.name + ".tar")
    ctx.actions.run_shell(
        outputs = [out],
        inputs = ctx.files.kit_files + ctx.files.workspace_files + [ctx.file.config],
        tools = [ctx.attr.tool[DefaultInfo].files_to_run],
        # The kit is written for /kit, where the image keeps it, and owned by
        # uid 1000, which is who the arena's base runs a kit as. Sorted and
        # dated at the epoch so the layer is the same bytes for the same kit.
        command = """
set -euo pipefail
kit="$(mktemp -d)/kit"
ARENA_KIT_FILES="$6" ARENA_KIT_REGISTRY="$7" "$1" kit \\
    --problem_config="$2" --out="$kit" --kit_path=/kit --prime_cache=false \\
    --server="$3" --http="$4" >/dev/null
tar --sort=name --mtime=@0 --owner=1000 --group=1000 --numeric-owner \\
    --transform='s,^\\.,kit,S' --create --file "$5" --directory "$kit" .
rm -rf "$(dirname "$kit")"
""",
        arguments = [
            ctx.executable.tool.path,
            ctx.file.config.path,
            ctx.attr.server,
            ctx.attr.http,
            out.path,
            " ".join([f.path for f in ctx.files.kit_files]),
            ctx.attr.registry,
        ],
        use_default_shell_env = True,
        # The tool tells the arena's sources from what was built beside them
        # by where a runfiles entry really lives. On a remote executor every
        # input is a file under bazel-out and that answer is always "built".
        execution_requirements = {"no-remote-exec": "1"},
        mnemonic = "ArenaKitTree",
        progress_message = "Writing the kit for %{label}",
    )
    return [DefaultInfo(files = depset([out]))]

kit_tree = rule(
    implementation = _kit_tree_impl,
    attrs = {
        "config": attr.label(mandatory = True, allow_single_file = True),
        "kit_files": attr.label_list(allow_files = True),
        # MODULE.bazel and what goes with it: the tool reads them from the
        # workspace root, which in an action is only what was declared.
        "workspace_files": attr.label_list(allow_files = True),
        "registry": attr.string(),
        "server": attr.string(default = "localhost:50051"),
        "http": attr.string(default = "localhost:8090"),
        # Target configuration, not exec: the tool carries the coordinator
        # and the worker as data, and an exec copy of it is a second build of
        # all of that for the sake of one action.
        "tool": attr.label(mandatory = True, executable = True, cfg = "target"),
    },
)
