# MCP servers

**arena** (`arena_mcp/server.py`) — submit a candidate, poll its build, read a
rival's source, queue matches, read the standings. A thin shim over the arena's
gRPC service (see `game_arena/ARENA.md`), so it needs a server running;
`ARENA_MCP_TARGET` points at it. `arena_submit` takes **paths, not file
contents**, so an agent never pastes back code it just wrote, and build failures
return extracted compiler errors rather than the bazel log. Start with
`arena_rules()`.

## Running under bazel (no venv needed)

A `py_binary` on the hermetic 3.12 toolchain with pinned pip deps from
`requirements.txt` (`pip.parse`, hub `@arena_mcp_pip_deps`):

```sh
bazel run //mcp_servers/arena_mcp:server
```

Under `bazel run` the server picks up the arena stubs from runfiles
(`//game_arena/proto:arena_py` — a `py_proto_library` plus a
grpc-python-plugin genrule) and uses `BUILD_WORKSPACE_DIRECTORY` as the repo
root (`ARENA_MCP_REPO_ROOT` still wins if set).

Register it with your agent by pointing at that command, e.g.

```json
{
  "mcpServers": {
    "arena": {
      "command": "bazel",
      "args": ["run", "//mcp_servers/arena_mcp:server"],
      "cwd": "/path/to/game-arena",
      "env": { "ARENA_MCP_TARGET": "localhost:50051" }
    }
  }
}
```

## Setup / reinstall (venv flow)

Fallback for machines without a working bazel, kept for reference. The venv is
git-ignored. If this system lacks `ensurepip`, use `virtualenv`. `mcp` must stay
on 1.x — the server imports `mcp.server.fastmcp`, removed in mcp 2.0.

```sh
python3 -m virtualenv mcp_servers/.venv
mcp_servers/.venv/bin/pip install "mcp>=1.0,<2" "protobuf==7.35.1" \
    "grpcio>=1.60" "grpcio-tools>=1.60"
mcp_servers/arena_mcp/make_stubs.sh
```

(`requirements.txt` has the same set, fully pinned, for bazel's `pip.parse` —
keep it in sync when bumping the venv install. The stub script is only for this
flow; the bazel flow generates its own.)
