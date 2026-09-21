#!/bin/bash
# Pushes this problem's kit image, runs its tournament on this host, and opens
# a participant's shell in the kit. Leaving the shell stops the tournament.
#   ./deploy.sh [name]      who you are in the tournament; default $USER
set -euo pipefail

bazel run //:kit_image_push -- --repository=registry.takumi.city/knapsack-kit --tag=1

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
    --clients="$HOME/.arena/knapsack/clients.textproto" | grep -oE '[A-Za-z0-9_-]{40,}' | head -1)"
kill -HUP "$(cat "$HOME/.arena/knapsack/problem_server.pid")" && sleep 1

# Inside: arena_cli submit --wait
docker run -it --rm --pull=always --network host \
    -e ARENA_NAME="$NAME" -e ARENA_TOKEN="$TOKEN" registry.takumi.city/knapsack-kit:1
