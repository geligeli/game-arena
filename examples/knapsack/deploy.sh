#!/bin/bash
# ./deploy.sh [name]: pushes the kit image, runs the tournament and a worker
# here, and opens a kit shell as name (default $USER); leaving it stops them.
set -euo pipefail

bazel run //:kit_image_push -- --repository=registry.takumi.city/knapsack-kit --tag=1

LOG="$(mktemp)"
bazel run //:tournament >"$LOG" 2>&1 &
trap 'kill $(jobs -p)' EXIT

# More workers, here or on any host with docker, are more capacity.
bazel run //:sandbox_image_load
bazel run @game_arena//game_arena/sandbox/worker:sandbox_worker -- \
    --server=localhost:50051 >>"$LOG" 2>&1 &

until grep -q "Attached to" "$LOG"; do sleep 1; done
NAME="${1:-$USER}"
TOKEN="$(bazel run @game_arena//game_arena/tools:arena_admin -- mint --overwrite --client_id="$NAME" \
    --clients="$HOME/.arena/knapsack/clients.textproto")"

docker run -it --rm --pull=always --network host \
    -e ARENA_SERVER=localhost:50051 -e ARENA_NAME="$NAME" -e ARENA_TOKEN="$TOKEN" registry.takumi.city/knapsack-kit:1
