#!/bin/bash
# Pushes this problem's kit image, primed, runs its tournament on this host,
# and opens a participant's shell in the kit. Leaving the shell stops the
# tournament.
#   ./deploy.sh [name]      who you are in the tournament; default $USER
set -euo pipefail

# Primed: dependencies vendored and the cache warm, through prime.bazelrc.
bazel run //:kit_image_issue -- --image=registry.takumi.city/connect4-kit:1 --push \
    --prime_bazelrc=prime.bazelrc

# The coordinator, as a process. It builds and runs nothing.
LOG="$(mktemp)"
bazel run //:tournament >"$LOG" 2>&1 &
trap 'kill $(jobs -p)' EXIT

# A worker, as another -- one more of these, here or on any host with docker,
# is more capacity -- and the sandbox image it builds and runs submissions in.
bazel run //:sandbox_image_load
bazel run @game_arena//game_arena/sandbox/worker:sandbox_worker -- \
    --server=localhost:50051 >>"$LOG" 2>&1 &

# Up once the worker has attached. A token is only read once the coordinator
# is up to be told about it.
until grep -q "attached with" "$LOG"; do sleep 1; done
NAME="${1:-$USER}"
TOKEN="$(bazel run @game_arena//game_arena/tools:arena_admin -- mint --client_id="$NAME" \
    --clients="$HOME/.arena/connect4/clients.textproto" | grep -oE '[A-Za-z0-9_-]{40,}' | head -1)"
kill -HUP "$(cat "$HOME/.arena/connect4/problem_server.pid")" && sleep 1

# Inside: arena_cli submit --wait, arena_cli spar <someone>
docker run -it --rm --pull=always --network host \
    -e ARENA_NAME="$NAME" -e ARENA_TOKEN="$TOKEN" registry.takumi.city/connect4-kit:1
