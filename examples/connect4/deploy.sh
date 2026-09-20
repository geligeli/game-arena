#!/bin/bash
# Pushes this problem's kit image, primed, runs its tournament on this host,
# and opens a participant's shell in the kit. Leaving the shell stops the
# tournament.
set -euo pipefail

# Primed: dependencies vendored and the cache warm, through prime.bazelrc.
bazel run //:kit_image_issue -- --image=registry.takumi.city/connect4-kit:1 --push \
    --prime_bazelrc=prime.bazelrc --out="$(mktemp -d)"

# The coordinator and a sandbox_worker (--workers=N for more), as processes;
# it makes the sandbox image the worker builds and runs submissions in.
LOG="$(mktemp)"
bazel run //:tournament >"$LOG" 2>&1 &
trap 'kill $!' EXIT

# Up once the worker has attached. A token is only read once the coordinator
# is up to be told about it.
until grep -q "attached with" "$LOG"; do sleep 1; done
TOKEN="$(bazel run @game_arena//game_arena/tools:arena_admin -- mint --client_id="$USER-$$" \
    --clients="$HOME/.arena/connect4/clients.textproto" | grep -oE '[A-Za-z0-9_-]{40,}' | head -1)"
kill -HUP "$(cat "$HOME/.arena/connect4/problem_server.pid")" && sleep 1

# Inside: arena_cli submit --name="my bot" --wait
docker run -it --rm --pull=always --network host -e ARENA_TOKEN="$TOKEN" \
    registry.takumi.city/connect4-kit:1
