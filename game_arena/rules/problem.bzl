"""A problem repository becomes a tournament with one macro call.

    load("@game_arena//game_arena/rules:problem.bzl", "arena_problem")

    arena_problem(
        name = "connect4",
        config = "problem.textproto",
        registry = "//game:registry",              # match problems only
        kit_files = ["//game:kit", "//bots:kit"],  # what a participant gets
    )

That call defines, in the calling package:

  :match_referee, :broker_server, :random_client
      the registry linked with the arena's entry points (only with `registry`)
  :config_test     `bazel test` -- the config parses and is consistent
  :tournament      `bazel run //:tournament -- [--no_container]` -- a coordinator
                   and local workers, on this checkout
  :kit             `bazel run //:kit -- --out=DIR --server=HOST:PORT --mint=ID`
                   -- a participant's workspace: kit_files, the arena's CLI and
                   MCP server, a README and a token
  :sandbox_image   `bazel run //:sandbox_image` -- the problem's offline sandbox
                   image, from sandbox.image in the config
  :<name>          a filegroup of every binary a tournament needs, so
                   `bazel build //:<name>` builds all of them

Every target is thin sugar over `@game_arena//game_arena/tools:arena_tournament`;
the tool takes the same flags without the macro. Nothing here names a problem:
the labels come from the caller.
"""

load("@rules_cc//cc:cc_binary.bzl", "cc_binary")
load("@rules_shell//shell:sh_binary.bzl", "sh_binary")
load("@rules_shell//shell:sh_test.bzl", "sh_test")

# Label() resolves against this file's repository, so a consumer loading this
# macro gets @game_arena's targets whatever it called the module.
_TOOL = Label("//game_arena/tools:arena_tournament")
_RUN = Label("//game_arena/rules:run_tool.sh")
_REFEREE_MAIN = Label("//game_arena/referee:referee_main")
_BROKER_MAIN = Label("//game_arena/referee:broker_server_main")
_RANDOM_CLIENT_MAIN = Label("//game_arena/client:random_client_main")
_TOURNAMENT_BINARIES = [
    Label("//game_arena/server:problem_server"),
    Label("//game_arena/sandbox/worker:sandbox_worker"),
    Label("//game_arena/tools:arena_admin"),
    Label("//game_arena/tools:arena_cli"),
    Label("//mcp_servers/arena_mcp:server"),
]

def arena_problem(name, config, registry = None, kit_files = [], visibility = None):
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
      visibility: applied to every generated target.
    """
    tool = str(_TOOL)
    run = str(_RUN)

    if registry:
        for target, main in [
            ("match_referee", _REFEREE_MAIN),
            ("broker_server", _BROKER_MAIN),
            ("random_client", _RANDOM_CLIENT_MAIN),
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
    sh_binary(
        name = "tournament",
        srcs = [run],
        data = base_data,
        args = base_args + ["up", config_arg],
        visibility = visibility,
    )
    sh_binary(
        name = "sandbox_image",
        srcs = [run],
        data = base_data,
        args = base_args + ["image", config_arg],
        visibility = visibility,
    )

    # The kit's file list travels in the environment rather than in args, so a
    # participant's own `-- --out=...` arguments are not mixed in with it.
    kit_args = base_args + ["kit", config_arg]
    if registry:
        kit_args.append("--registry=%s" % registry)
    sh_binary(
        name = "kit",
        srcs = [run],
        data = base_data + kit_files,
        args = kit_args,
        env = {
            "ARENA_KIT_FILES": " ".join(["$(rootpaths %s)" % f for f in kit_files]),
        },
        visibility = visibility,
    )

    native.filegroup(
        name = name,
        srcs = [str(label) for label in _TOURNAMENT_BINARIES] + [tool] +
               ([":match_referee"] if registry else []),
        visibility = visibility,
    )
