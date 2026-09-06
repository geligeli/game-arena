#!/bin/sh
# Regenerates the arena's Python gRPC stubs into _pb/.
#
# Only needed for the venv flow (see mcp_servers/README.md): under bazel the
# stubs come from //game_mcts/tournament_server/proto:arena_py. Re-run after
# changing game_mcts/tournament_server/proto/arena.proto.
set -e

here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
venv="$repo/mcp_servers/.venv"

mkdir -p "$here/_pb"
"$venv/bin/python" -m grpc_tools.protoc \
    -I"$repo" \
    --python_out="$here/_pb" \
    --grpc_python_out="$here/_pb" \
    game_mcts/tournament_server/proto/arena.proto

# protoc emits package-relative imports; make the generated tree importable.
find "$here/_pb" -type d -exec touch {}/__init__.py \;
echo "stubs regenerated in $here/_pb"
