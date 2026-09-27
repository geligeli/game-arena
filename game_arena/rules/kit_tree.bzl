"""A kit as a tar of /kit: `arena_tournament kit` in an action, unprimed and with no token."""

def _kit_tree_impl(ctx):
    out = ctx.actions.declare_file(ctx.label.name + ".tar")
    ctx.actions.run_shell(
        outputs = [out],
        inputs = ctx.files.kit_files + ctx.files.workspace_files + [ctx.file.config],
        tools = [ctx.attr.tool[DefaultInfo].files_to_run],
        # uid 1000 is who kit_base runs as; sorted and dated at the epoch for reproducible bytes.
        command = """
set -euo pipefail
kit="$(mktemp -d)/kit"
ARENA_KIT_FILES="$4" ARENA_KIT_REGISTRY="$5" "$1" kit \\
    --problem_config="$2" --out="$kit" --kit_path=/kit --prime_cache=false \\
    >/dev/null
tar --sort=name --mtime=@0 --owner=1000 --group=1000 --numeric-owner \\
    --transform='s,^\\.,kit,S' --create --file "$3" --directory "$kit" .
rm -rf "$(dirname "$kit")"
""",
        arguments = [
            ctx.executable.tool.path,
            ctx.file.config.path,
            out.path,
            " ".join([f.path for f in ctx.files.kit_files]),
            ctx.attr.registry,
        ],
        use_default_shell_env = True,
        # The tool tells sources from outputs by where a runfile lives; remotely, all look built.
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
        # What the tool reads from the workspace root, which in an action is only what was declared.
        "workspace_files": attr.label_list(allow_files = True),
        "registry": attr.string(),
        # Target, not exec: an exec copy would build the coordinator and worker it carries twice.
        "tool": attr.label(mandatory = True, executable = True, cfg = "target"),
    },
)
