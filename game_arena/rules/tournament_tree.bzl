"""A tournament's files as a tar: the layer a tournament image is made of.

    bazel build //:tournament_image

Laid out as `arena_tournament` expects to find them installed ($ARENA_HOME is
/opt/arena): the arena's binaries where they are in its tree, the kit surface
as sources for the kits it writes, the built kit image and regctl for the ones
it hands out as images, the docker CLI its workers reach the socket with, and
the problem's config and kit files. The binaries are built from the problem's
workspace, so they are the arena the problem depends on.

What is not here is the problem's repository: a git history is not something a
build action can be handed. A problem whose `repo.url` is a path has it added
outside the build (the macro's tournament_image_bundle target); one whose
`repo.url` is a remote needs nothing more than this.
"""

def _repo_relative(f):
    """A file's path inside its own repository, whichever repository that is."""
    path = f.short_path
    if path.startswith("../"):
        return path.split("/", 2)[2]
    return path

def _tournament_tree_impl(ctx):
    out = ctx.actions.declare_file(ctx.label.name + ".tar")

    # dest <tab> source, one per line; a directory (the kit image's layout) is
    # copied whole.
    entries = []
    inputs = []
    links = []
    for binary in ctx.attr.binaries:
        f = binary[DefaultInfo].files_to_run.executable
        dest = "opt/arena/" + _repo_relative(f)
        entries.append((dest, f))
        links.append(("usr/local/bin/" + f.basename, "/" + dest))
    for f in ctx.files.kit_surface:
        entries.append(("opt/arena/src/game_arena/" + _repo_relative(f), f))
    for f in ctx.files.problem_files:
        entries.append(("opt/arena/problem/" + _repo_relative(f), f))
    entries.append(("opt/arena/bin/regctl", ctx.file.regctl))
    entries.append(("usr/local/bin/docker", ctx.file.docker))
    entries.append(("opt/arena/kit_image", ctx.file.kit_image))

    manifest = ctx.actions.declare_file(ctx.label.name + ".manifest")
    ctx.actions.write(
        manifest,
        "".join(["F\t%s\t%s\n" % (dest, f.path) for dest, f in entries] +
                ["L\t%s\t%s\n" % (dest, target) for dest, target in links]),
    )
    inputs = [f for _, f in entries] + [manifest]

    ctx.actions.run_shell(
        outputs = [out],
        inputs = inputs,
        # root's, like everything else outside /kit and /var/arena: the
        # coordinator and its workers run as root, because the socket is
        # root's. Sorted and dated at the epoch so the layer is the same bytes
        # for the same inputs.
        command = """
set -euo pipefail
stage="$(mktemp -d)"
while IFS=$'\\t' read -r kind dest source; do
  mkdir -p "$stage/$(dirname "$dest")"
  if [[ "$kind" == L ]]; then
    ln -s "$source" "$stage/$dest"
  else
    cp -rL "$source" "$stage/$dest"
  fi
done < "$1"
chmod -R u+w "$stage"
tar --sort=name --mtime=@0 --owner=0 --group=0 --numeric-owner \\
    --create --file "$2" --directory "$stage" opt/arena usr/local/bin
rm -rf "$stage"
""",
        arguments = [manifest.path, out.path],
        use_default_shell_env = True,
        mnemonic = "ArenaTournamentTree",
        progress_message = "Laying out the tournament for %{label}",
    )
    return [DefaultInfo(files = depset([out]))]

tournament_tree = rule(
    implementation = _tournament_tree_impl,
    attrs = {
        # Target configuration: these run in the image, not in this build.
        "binaries": attr.label_list(cfg = "target"),
        "kit_surface": attr.label_list(allow_files = True),
        "problem_files": attr.label_list(allow_files = True),
        "kit_image": attr.label(mandatory = True, allow_single_file = True),
        "regctl": attr.label(mandatory = True, allow_single_file = True),
        "docker": attr.label(mandatory = True, allow_single_file = True),
    },
)

def _tournament_env_impl(ctx):
    out = ctx.actions.declare_file(ctx.label.name + ".env")
    env = {
        "ARENA_HOME": "/opt/arena",
        "ARENA_STATE_DIR": "/var/arena",
        "ARENA_PROBLEM_CONFIG": _repo_relative(ctx.file.config),
        "ARENA_VOLUME_PREFIX": ctx.attr.volume_prefix,
        "ARENA_KIT_FILES": " ".join([_repo_relative(f) for f in ctx.files.kit_files]),
        "ARENA_KIT_REGISTRY": ctx.attr.registry,
        "ARENA_KIT_BASE": "/opt/arena/kit_image",
        "ARENA_REGCTL": "/opt/arena/bin/regctl",
    }
    ctx.actions.write(out, "".join(["%s=%s\n" % (k, v) for k, v in env.items()]))
    return [DefaultInfo(files = depset([out]))]

# The environment a tournament image runs with, as the KEY=VALUE file
# oci_image takes. A rule rather than a dict in the macro: which files a
# kit_files label names, and where the config is, are only known here.
#
# ARENA_VOLUME_PREFIX: a container's hostname is its id, so the default
# (arena-<hostname>) would name fresh cache volumes every restart. The kit's
# files and registry travel with the image because `kit` runs in it without
# the macro; ARENA_KIT_BASE is what makes `kit --image` there the same few
# seconds it is from a checkout.
tournament_env = rule(
    implementation = _tournament_env_impl,
    attrs = {
        "config": attr.label(mandatory = True, allow_single_file = True),
        "kit_files": attr.label_list(allow_files = True),
        "registry": attr.string(),
        "volume_prefix": attr.string(mandatory = True),
    },
)
