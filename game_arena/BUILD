load("@rules_shell//shell:sh_test.bzl", "sh_test")

package(default_visibility = ["//visibility:public"])

# What a participant's kit builds: the referee `arena_cli spar` plays with, the
# session contract their own code compiles against, the broker protocol, and
# the MCP server their agent drives. Everything these
# reach has to be inside //:kit_surface, because that is all a kit gets.
_KIT_TARGETS = [
    "//game_arena/referee:game_registry",
    "//game_arena/referee:game_session",
    "//game_arena/referee:referee_main",
    "//game_arena/cli:arena_cli",
    "//game_arena/client:play_loop",
    "//game_arena/proto:tournament_broker_cc_grpc",
    "//mcp_servers/arena_mcp:server",
]

genquery(
    name = "kit_surface_deps",
    expression = " + ".join(["deps(%s)" % target for target in _KIT_TARGETS]),
    opts = ["--output=label"],
    scope = _KIT_TARGETS,
)

sh_test(
    name = "kit_surface_test",
    srcs = ["kit_surface_test.sh"],
    args = ["$(location :kit_surface_deps)"],
    data = [":kit_surface_deps"],
)
