"""A problem repository becomes a tournament with one macro call.

    load("@game_arena//game_arena/rules:problem.bzl", "arena_problem")

    arena_problem(
        name = "connect4",
        config = "problem.textproto",
        registry = "//game:registry",              # match problems only
        kit_files = ["//game:kit", "//bots:kit"],  # what a participant gets
    )

That call defines, in the calling package:

  :match_referee   the registry linked with the arena's referee (only with
                   `registry`): one match, as a worker or `arena_cli spar` runs it
  :config_test     `bazel test` -- the config parses and is consistent
  :tournament      `bazel run //:tournament` -- the coordinator, and nothing
                   else: it builds and runs nothing. A worker is a process of
                   its own (`sandbox_worker --server=HOST:PORT`), on any host
                   with docker and the sandbox image
  :kit             `bazel run //:kit -- --out=DIR --server=HOST:PORT --mint=ID`
                   -- a participant's workspace: kit_files, the arena's kit
                   surface vendored as ./arena, arena_cli as a program, an MCP
                   server, a README and a token. Their own environment, with
                   whatever access they give it; only what they submit runs
                   sandboxed
  :kit_image       `bazel build //:kit_image` -- the same kit as an image: the
                   kit's tree at /kit, stacked on `kit_base` by rules_oci. A
                   build output like any other, so making one needs no docker
                   -- only, the first time, the registry the base is pulled
                   from. It is anyone's: there is no token in it, and nothing
                   is built in it yet. `docker run -e ARENA_TOKEN=...` is one
                   way to hand a participant theirs
  :kit_image_load  `bazel run //:kit_image_load` -- that image, into the local
                   docker daemon, tagged `<name>-kit:latest`
  :kit_image_push  `bazel run //:kit_image_push` -- that image, to
                   `kit_repository` (`-- --repository=REG/NAME` to override)
  :kit_image_issue `bazel run //:kit_image_issue -- --image=TAG [--push]
                   [--mint=ID | --token=T] [--prime_cache=false]` -- what a
                   build cannot add to that image, added outside one: the
                   kit's dependencies vendored and its cache primed (bazel,
                   run on the kit), and a participant's token (a secret, which
                   a remote cache would keep). Each is a layer on the built
                   image and either can be left out; with only the token it
                   takes seconds
  :play            `bazel run //:play` -- the tournament in
                   the background, a kit minted for you, and a shell in it with
                   ARENA_SERVER and ARENA_TOKEN set. Leaving the shell stops
                   everything. The dev loop
  :sandbox_image   `bazel build //:sandbox_image` -- the problem's sandbox
                   image: the arena's sandbox base, the arena's sources, and
                   the problem's tree (the root package's files and `tree`) at
                   /workspace. `:sandbox_image_load` and `:sandbox_image_push`
                   deliver it as sandbox.image in the config names it. Nothing
                   is vendored into it: the first build in a slot fetches what
                   the problem resolves, so sandbox.allow_build_network
  :<name>          a filegroup of every binary a tournament needs, so
                   `bazel build //:<name>` builds all of them

Every target is thin sugar over `@game_arena//game_arena/tools:arena_tournament`;
the tool takes the same flags without the macro. Nothing here names a problem:
the labels come from the caller.
"""

load("@rules_cc//cc:cc_binary.bzl", "cc_binary")
load("@rules_oci//oci:defs.bzl", "oci_image", "oci_load", "oci_push")
load("@rules_shell//shell:sh_binary.bzl", "sh_binary")
load("@rules_shell//shell:sh_test.bzl", "sh_test")
load(":kit_tree.bzl", "kit_tree")
load(":sandbox_tree.bzl", "sandbox_tree")

# Label() resolves against this file's repository, so a consumer loading this
# macro gets @game_arena's targets whatever it called the module.
_TOOL = Label("//game_arena/tools:arena_tournament")
_RUN = Label("//game_arena/rules:run_tool.sh")
_REFEREE_MAIN = Label("//game_arena/referee:referee_main")
_KIT_BASE = Label("//game_arena/image:kit_base")
_SANDBOX_BASE = Label("//game_arena/image:sandbox_base")
_KIT_SURFACE = Label("//:kit_surface")
_SANDBOX_SURFACE = Label("//:sandbox_surface")
_REGCTL = Label("//game_arena/image:regctl")
_TOURNAMENT_BINARIES = [
    Label("//game_arena/server:problem_server"),
    Label("//game_arena/sandbox/worker:sandbox_worker"),
    Label("//game_arena/tools:arena_admin"),
    Label("//game_arena/cli:arena_cli"),
    Label("//mcp_servers/arena_mcp:server"),
]

def arena_problem(
        name,
        config,
        registry = None,
        kit_files = [],
        tree = [],
        kit_base = None,
        kit_server = "localhost:50051",
        kit_http = "localhost:8090",
        kit_repository = None,
        visibility = None):
    """Defines the tournament targets for one problem. See the module docstring.

    Args:
      name: the problem's name; also the filegroup of every tournament binary.
      config: the problem's .textproto (see @game_arena//game_arena/proto:problem.proto).
      registry: label of the cc_library (alwayslink) defining GameRegistry(), for
        a match problem. None for a graded problem.
      kit_files: labels of the files a participant receives -- the API a
        solution is written against, the harness, a reference solution, and the
        BUILD files that build them. Nothing else leaves the repo. Filegroups
        and globs work; every file must be a source file of this repository.
      tree: labels of the problem's files outside the root package, which a
        glob here cannot reach: one `filegroup(srcs = glob(["**"]))` per
        package. With the root package's own files they are the tree a
        submission is built on, in the sandbox image.
      kit_server: the arena's address as a participant reaches it, written
        into the kit image (`docker run -e ARENA_SERVER=...` overrides it).
        The default is only right for a container sharing the coordinator's
        network namespace.
      kit_http: the leaderboard's address, likewise.
      kit_repository: where `:kit_image_push` pushes, e.g.
        "registry.example.com/connect4-kit". Without it that target is still
        defined, and takes `-- --repository=...`.
      kit_base: label of the OCI image layout a kit image is layered onto.
        Default: the arena's, which is bazel, git and python3 and nothing of a
        problem. A problem whose participants need more builds its own
        `oci_image` with `base = "@game_arena//game_arena/image:kit_base"`
        and names it here; it has to keep that base's user (uid 1000), who
        owns /kit.
      visibility: applied to every generated target.
    """
    tool = str(_TOOL)
    run = str(_RUN)

    if registry:
        for target, main in [
            ("match_referee", _REFEREE_MAIN),
        ]:
            cc_binary(
                name = target,
                deps = [registry, str(main)],
                visibility = visibility,
            )

    base_args = [
        "$(rootpath %s)" % tool,
    ]
    config_arg = "--problem_config=$(rootpath %s)" % config
    base_data = [config, tool]

    sh_test(
        name = "config_test",
        srcs = [run],
        data = base_data,
        args = base_args + ["check", config_arg],
        visibility = visibility,
    )
    # The kit's file list travels in the environment rather than in args, so a
    # participant's own `-- --out=...` arguments are not mixed in with it.
    kit_env = {
        "ARENA_KIT_FILES": " ".join(["$(rootpaths %s)" % f for f in kit_files]),
        "ARENA_KIT_REGISTRY": registry or "",
    }
    sh_binary(
        name = "tournament",
        srcs = [run],
        data = base_data,
        args = base_args + ["up", config_arg],
        visibility = visibility,
    )

    # The sandbox image, built: the problem's tree where a job's volume is
    # mounted, and the arena's sources where the image's bazelrc overrides
    # game_arena to -- a path on this host means nothing in a sandbox. manual,
    # like every target that carries a base: `//...` must not need a registry.
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

    # Delivered under the name the config gives it, which is the one a worker
    # asks its daemon for. A name with no registry in it is a local tag, and
    # pushing one would go to Docker Hub.
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

    sh_binary(
        name = "kit",
        srcs = [run],
        data = base_data + kit_files,
        args = base_args + ["kit", config_arg],
        env = kit_env,
        visibility = visibility,
    )
    # The kit as an image, built. manual, all of it: these are the targets
    # that need a registry -- the base is pulled the first time -- and `//...`
    # must not. That covers `bazel test //...` here, and the `bazel vendor
    # //...` a sandbox image is made with, which would otherwise carry the
    # base's layers into every sandbox.
    # MODULE.bazel and what goes with it: what the tool reads from the
    # workspace root, which in an action is only what was declared.
    workspace_files = native.glob(
        ["MODULE.bazel", "MODULE.bazel.lock", ".bazelversion", ".bazelrc"],
        allow_empty = True,
    )
    kit_tree(
        name = "kit_tree",
        config = config,
        kit_files = kit_files,
        registry = registry or "",
        server = kit_server,
        http = kit_http,
        tool = tool,
        workspace_files = workspace_files,
        tags = ["manual"],
        visibility = visibility,
    )
    oci_image(
        name = "kit_image",
        base = kit_base or str(_KIT_BASE),
        tars = [":kit_tree"],
        env = {"ARENA_SERVER": kit_server},
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
        repository = kit_repository or "unset.invalid/pass--repository",
        remote_tags = ["latest"],
        tags = ["manual"],
        visibility = visibility,
    )

    # What a build cannot add to :kit_image, added to it outside one.
    sh_binary(
        name = "kit_image_issue",
        srcs = [run],
        data = base_data + kit_files + [":kit_image", str(_REGCTL)],
        args = base_args + ["kit", config_arg],
        env = kit_env | {
            "ARENA_KIT_BASE": "$(rootpath :kit_image)",
            "ARENA_REGCTL": "$(rootpath %s)" % _REGCTL,
        },
        tags = ["manual"],
        visibility = visibility,
    )
    sh_binary(
        name = "play",
        srcs = [run],
        data = base_data + kit_files,
        args = base_args + ["play", config_arg],
        # `play` makes its worker's sandbox image by running the target that
        # does, rather than carrying that target's base itself.
        env = kit_env | {
            "ARENA_SANDBOX_LOAD_TARGET": "//%s:sandbox_image_load" % native.package_name(),
        },
        visibility = visibility,
    )

    native.filegroup(
        name = name,
        srcs = [str(label) for label in _TOURNAMENT_BINARIES] + [tool] +
               ([":match_referee"] if registry else []),
        visibility = visibility,
    )
