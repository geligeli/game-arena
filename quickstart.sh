#!/usr/bin/bash
set -euo pipefail
TAG=latest
REGISTRY=registry.takumi.city/connect4-kit
NAME_P1=alice
NAME_P2=bob

DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" >/dev/null 2>&1 && pwd )"
SESSION="game-arena-c4"
TARGET_DIR=${DIR}/examples/connect4

# An unchanged Dockerfile is a cache hit and the same digest, so a pin only moves when it does.
repin() {  # <old digest> <pushed tag> <files...>
  local new
  new=$(docker inspect --format '{{range .RepoDigests}}{{println .}}{{end}}' "$2" \
    | grep "^${2%:*}@" | cut -d@ -f2)
  sed -i "s/$1/$new/" "${@:3}"
}
BAZEL=$(cat ${DIR}/.bazelversion)
BASE=registry.takumi.city/game-arena-base:noble-bazel${BAZEL}-nano
docker build --provenance=false --sbom=false --push \
 --build-arg BAZEL_VERSION=${BAZEL} -t ${BASE} ${DIR}/docker/base
repin $(grep -o 'sha256:[0-9a-f]*' ${DIR}/docker/coding-agents/Dockerfile) ${BASE} \
 ${DIR}/MODULE.bazel ${DIR}/docker/coding-agents/Dockerfile
AGENTS=registry.takumi.city/game-arena-kit-agents:latest
docker build --provenance=false --sbom=false --push -t ${AGENTS} ${DIR}/docker/coding-agents
repin $(grep -A2 'name = "kit_agents"' ${TARGET_DIR}/MODULE.bazel | grep -o 'sha256:[0-9a-f]*') \
 ${AGENTS} ${TARGET_DIR}/MODULE.bazel

cd $TARGET_DIR

# Pulled back, so the worker here does not run a stale tag.
bazel run -c opt //:sandbox_image_issue -- --prime_bazelrc=prime.bazelrc --push
docker pull registry.takumi.city/connect4-sandbox:latest

bazel run -c opt \
 //:kit_image_issue -- \
 --image=${REGISTRY}:${TAG} \
 --prime_bazelrc=prime.bazelrc \
 --push

mint() {
  bazel run @game_arena//game_arena/tools:arena_admin -- mint --client_id=$1 \
    --clients=$HOME/.arena/connect4/clients.textproto --overwrite
}
TOKEN_P1=$(mint ${NAME_P1})
TOKEN_P2=$(mint ${NAME_P2})

tmux new-session -d -s $SESSION -c $TARGET_DIR 'bazel run -c opt //:tournament'
tmux split-window -h -t $SESSION -c $TARGET_DIR 'bazel run -c opt  @game_arena//game_arena/sandbox/worker:sandbox_worker -- --server=localhost:50051'

kit_pane() {  # <split flag> <name> <token>
  tmux split-window $1 -t $SESSION -c $TARGET_DIR "docker run -it --rm --pull=always --network host \
 -e ARENA_SERVER=localhost:50051 -e ARENA_NAME=$2 -e ARENA_TOKEN=$3 -e ARENA_RESTORE=1 -e CLAUDE_CODE_OAUTH_TOKEN ${REGISTRY}:${TAG}"
}
kit_pane -vf ${NAME_P1} ${TOKEN_P1}
kit_pane -h ${NAME_P2} ${TOKEN_P2}

tmux attach-session -t $SESSION
