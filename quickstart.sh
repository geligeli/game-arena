#!/usr/bin/bash
TAG=latest
REGISTRY=registry.takumi.city/connect4-kit
NAME_P1=alice
NAME_P2=bob


DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" >/dev/null 2>&1 && pwd )"
SESSION="game-arena-c4"
TARGET_DIR=${DIR}/examples/connect4

cd $TARGET_DIR

# used by the worker server side: dependencies vendored, cache primed. Pulled
# back, so the worker here does not run a stale tag.
bazel run -c opt //:sandbox_image_issue -- --prime_bazelrc=prime.bazelrc --push
docker pull registry.takumi.city/connect4-sandbox:latest

bazel run -c opt \
 //:kit_image_issue -- \
 --image=${REGISTRY}:${TAG} \
 --prime_bazelrc=prime.bazelrc \
 --push


TOKEN_P1=$( bazel run @game_arena//game_arena/tools:arena_admin \
 -- mint \
 --client_id=${NAME_P1} \
 --clients=$HOME/.arena/connect4/clients.textproto \
 --overwrite )


TOKEN_P2=$( bazel run @game_arena//game_arena/tools:arena_admin \
 -- mint \
 --client_id=${NAME_P2} \
 --clients=$HOME/.arena/connect4/clients.textproto \
 --overwrite )

tmux new-session -d -s $SESSION -c $TARGET_DIR 'bazel run -c opt //:tournament'
tmux split-window -h -t $SESSION -c $TARGET_DIR 'bazel run -c opt  @game_arena//game_arena/sandbox/worker:sandbox_worker -- --server=localhost:50051'

tmux split-window -vf -t $SESSION -c $TARGET_DIR "docker run \
 -it \
 --rm \
 --pull=always \
 --network host \
 -e ARENA_SERVER=localhost:50051 \
 -e ARENA_NAME=${NAME_P1} \
 -e ARENA_TOKEN=${TOKEN_P1} \
 -e CLAUDE_CODE_OAUTH_TOKEN \
 ${REGISTRY}:${TAG}
"

tmux split-window -h -t $SESSION -c $TARGET_DIR "docker run \
 -it \
 --rm \
 --pull=always \
 --network host \
 -e ARENA_SERVER=localhost:50051 \
 -e ARENA_NAME=${NAME_P2} \
 -e ARENA_TOKEN=${TOKEN_P2} \
 ${REGISTRY}:${TAG}
"

# Attach to the session
tmux attach-session -t $SESSION


