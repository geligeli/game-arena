"""A problem repository becomes a tournament with one macro call.

    arena_problem(
        name = "connect4",
        config = "problem.textproto",
        registry = "//game:registry",              # match problems only
        kit_files = ["//game:kit", "//bots:kit"],  # what a participant gets
    )

defines, in the calling package:

  :match_referee        the registry linked with the arena's referee (with `registry`)
  :config_test          the config parses and is consistent
  :tournament           the coordinator; a worker is a process of its own
  :kit                  `-- --out=DIR --server=HOST:PORT --mint=ID`: a participant's workspace
  :kit_image            that kit on kit_base; no token, no address, nothing built
  :kit_image_load       into the local daemon as <name>-kit:latest
  :kit_image_push       `-- --repository=REG/NAME`
  :kit_image_issue      `-- --image=TAG [--push]`: the kit image, vendored and primed
  :play                 the tournament, a minted kit and a shell in it
  :sandbox_image        the sandbox base, the arena's sources and this tree at /workspace
  :sandbox_image_load   into the local daemon as sandbox.image names it
  :sandbox_image_push   to where sandbox.image names
  :sandbox_image_issue  `-- [--push]`: the sandbox image, vendored and primed
  :<name>               every binary a tournament needs

Each runnable target is arena_tournament with the same flags.
"""

load("@rules_cc//cc:cc_binary.bzl", "cc_binary")
load("@rules_oci//oci:defs.bzl", "oci_image", "oci_load", "oci_push")
load("@rules_shell//shell:sh_binary.bzl", "sh_binary")
load("@rules_shell//shell:sh_test.bzl", "sh_test")
load(":kit_tree.bzl", "kit_tree")
load(":sandbox_tree.bzl", "sandbox_tree")

# Label() resolves in @game_arena, whatever a consumer calls the module.
_TOOL = Label("//game_arena/tools:arena_tournament")
_RUN = Label("//game_arena/rules:run_tool.sh")
_REFEREE_MAIN = Label("//game_arena/referee:referee_main")
_KIT_BASE = Label("//game_arena/image:kit_base")
_KIT_ENTRYPOINT = Label("//game_arena/image:kit_entrypoint_layer")
_SANDBOX_BASE = Label("//game_arena/image:sandbox_base")
_SANDBOX_SURFACE = Label("//:sandbox_surface")
_REGCTL = Label("//game_arena/image:regctl")
_TOURNAMENT_BINARIES = [
    Label("//game_arena/server:problem_server"),
    Label("//game_arena/sandbox/worker:sandbox_worker"),
    Label("//game_arena/tools:arena_admin"),
    Label("//game_arena/cli:arena_cli"),
]

def _tool(rule, name, command, config, data = [], **kwargs):
    rule(
        name = name,
        srcs = [str(_RUN)],
        data = [config, str(_TOOL)] + data,
        args = ["$(rootpath %s)" % _TOOL, command, "--problem_config=$(rootpath %s)" % config],
        **kwargs
    )

def arena_problem(
        name,
        config,
        registry = None,
        kit_files = [],
        tree = [],
        kit_base = None,
        visibility = None):
    """Defines the tournament targets for one problem. See the module docstring.

    Args:
      registry: the alwayslink cc_library defining GameRegistry(); None for a graded problem.
      kit_files: every file a participant receives, all source files of this repository.
      tree: one `filegroup(srcs = glob(["**"]))` per package outside the root one.
      kit_base: in place of the arena's kit_base; build it on that one and keep its uid 1000.
    """
    if registry:
        cc_binary(
            name = "match_referee",
            deps = [registry, str(_REFEREE_MAIN)],
            visibility = visibility,
        )

    _tool(sh_test, "config_test", "check", config, visibility = visibility)

    # In the environment, not args, so a participant's own `-- --out=...` is not mixed in.
    kit_env = {
        "ARENA_KIT_FILES": " ".join(["$(rootpaths %s)" % f for f in kit_files]),
        "ARENA_KIT_REGISTRY": registry or "",
    }
    _tool(sh_binary, "tournament", "up", config, visibility = visibility)

    # manual, like every target that carries a base: `//...` must not need a registry.
    sandbox_tree(
        name = "sandbox_tree",
        srcs = native.glob(["**"], exclude = [".arena/**", ".bazelrc.local", "bazel-*/**"]) + tree,
        prefix = "workspace",
        tags = ["manual"],
        visibility = visibility,
    )
    sandbox_tree(
        name = "sandbox_arena",
        srcs = [str(_SANDBOX_SURFACE)],
        prefix = "opt/arena/src/game_arena",
        tags = ["manual"],
        visibility = visibility,
    )
    oci_image(
        name = "sandbox_image",
        base = str(_SANDBOX_BASE),
        tars = [":sandbox_arena", ":sandbox_tree"],
        tags = ["manual"],
        visibility = visibility,
    )

    # A name with no registry is a local tag, and pushing one would go to Docker Hub.
    image_ref = "ref=$$(sed -n '/^sandbox {/,/^}/s/^ *image: \"\\(.*\\)\"/\\1/p' $<); "
    native.genrule(
        name = "sandbox_image_tags",
        srcs = [config],
        outs = ["sandbox_image.tags.txt"],
        cmd = image_ref + "echo \"$$ref\" > $@",
        tags = ["manual"],
    )
    native.genrule(
        name = "sandbox_image_remote",
        srcs = [config],
        outs = ["sandbox_image.repository.txt", "sandbox_image.tag.txt"],
        cmd = image_ref + """
case "$${ref%%/*}" in
  *.*|*:*|localhost) ;;
  *) echo "sandbox.image ($$ref) names no registry: set it to REGISTRY/NAME:TAG" >&2; exit 1;;
esac
echo "$${ref%:*}" > $(location sandbox_image.repository.txt)
echo "$${ref##*:}" > $(location sandbox_image.tag.txt)
""",
        tags = ["manual"],
    )
    oci_load(
        name = "sandbox_image_load",
        image = ":sandbox_image",
        repo_tags = ":sandbox_image.tags.txt",
        tags = ["manual"],
        visibility = visibility,
    )
    oci_push(
        name = "sandbox_image_push",
        image = ":sandbox_image",
        repository_file = ":sandbox_image.repository.txt",
        remote_tags = ":sandbox_image.tag.txt",
        tags = ["manual"],
        visibility = visibility,
    )

    _tool(sh_binary, "kit", "kit", config, kit_files, env = kit_env, visibility = visibility)

    # What the tool reads from the workspace root, which in an action is only what was declared.
    workspace_files = native.glob(
        ["MODULE.bazel", "MODULE.bazel.lock", ".bazelversion", ".bazelrc"],
        allow_empty = True,
    )
    kit_tree(
        name = "kit_tree",
        config = config,
        kit_files = kit_files,
        registry = registry or "",
        tool = str(_TOOL),
        workspace_files = workspace_files,
        tags = ["manual"],
        visibility = visibility,
    )

    # The entrypoint here, not on the base: a problem may replace the base.
    oci_image(
        name = "kit_image",
        base = kit_base or str(_KIT_BASE),
        tars = [str(_KIT_ENTRYPOINT), ":kit_tree"],
        entrypoint = ["/usr/local/bin/kit_entrypoint"],
        cmd = ["bash"],
        tags = ["manual"],
        visibility = visibility,
    )
    oci_load(
        name = "kit_image_load",
        image = ":kit_image",
        repo_tags = [name + "-kit:latest"],
        tags = ["manual"],
        visibility = visibility,
    )
    oci_push(
        name = "kit_image_push",
        image = ":kit_image",
        repository = "unset.invalid/pass--repository",
        remote_tags = ["latest"],
        tags = ["manual"],
        visibility = visibility,
    )

    _tool(
        sh_binary,
        "kit_image_issue",
        "kit",
        config,
        kit_files + [":kit_image", str(_REGCTL)],
        env = kit_env | {
            "ARENA_KIT_BASE": "$(rootpath :kit_image)",
            "ARENA_REGCTL": "$(rootpath %s)" % _REGCTL,
        },
        tags = ["manual"],
        visibility = visibility,
    )
    _tool(
        sh_binary,
        "sandbox_image_issue",
        "sandbox",
        config,
        [":sandbox_image", ":sandbox_arena", ":sandbox_tree", str(_REGCTL)],
        env = {
            "ARENA_SANDBOX_BASE": "$(rootpath :sandbox_image)",
            "ARENA_SANDBOX_TARS": "$(rootpath :sandbox_arena) $(rootpath :sandbox_tree)",
            "ARENA_REGCTL": "$(rootpath %s)" % _REGCTL,
        },
        tags = ["manual"],
        visibility = visibility,
    )
    _tool(
        sh_binary,
        "play",
        "play",
        config,
        kit_files,
        # Run, not carried: carrying the base would put it in `//...`.
        env = kit_env | {
            "ARENA_SANDBOX_LOAD_TARGET": "//%s:sandbox_image_load" % native.package_name(),
            "ARENA_SANDBOX_ISSUE_TARGET": "//%s:sandbox_image_issue" % native.package_name(),
        },
        visibility = visibility,
    )

    native.filegroup(
        name = name,
        srcs = [str(label) for label in _TOURNAMENT_BINARIES] + [str(_TOOL)] +
               ([":match_referee"] if registry else []),
        visibility = visibility,
    )
