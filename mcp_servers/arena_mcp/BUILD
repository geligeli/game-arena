load("@rules_python//python:defs.bzl", "py_binary")

package(default_visibility = ["//visibility:public"])

# Arena MCP server. Run with: bazel run //mcp_servers/arena_mcp:server
# (stubs come from //game_arena/proto:arena_py; the venv +
# make_stubs.sh flow in mcp_servers/README.md still works unchanged).
py_binary(
    name = "server",
    srcs = ["server.py"],
    deps = [
        "//game_arena/proto:arena_py",
        "@mcp_pip_deps//grpcio",
        "@mcp_pip_deps//mcp",
    ],
)
