# syntax=docker/dockerfile:1.7
#
# A participant's kit as a docker image.
#
# Built by `arena_tournament kit --image=<tag>` with the kit directory as the
# build context: the workspace the kit subcommand wrote (the problem's
# kit_files, the arena's CLI and MCP server reachable through @game_arena, the
# README, the token) plus the toolchain to build it, with everything built
# once so the first command a participant runs is instant. This is their
# development environment, not a sandbox: it has whatever network and access
# `docker run` gives it, and only what they submit runs sandboxed.
#
#   docker run -it <tag>                                  a shell in /kit
#   docker run -i <tag> bazel run //:mcp_server           an MCP server on stdio
#   docker run -it <tag> bazel run //:arena_cli -- leaderboard
#
# The arena's address and the token are baked in as environment, so the tools
# work without sourcing arena.env, and `docker run -e ARENA_SERVER=...` moves
# the kit to another coordinator.
#
# Needs BuildKit (the default since Docker 23) for the named build context
# below. Build args:
#   BAZEL_VERSION   the bazel release to install; the tool reads .bazelversion
#   ARENA_SERVER, ARENA_HTTP, ARENA_TOKEN, ARENA_CLIENT_ID
#                   what the tool baked into arena.env and mcp.json

ARG BAZEL_VERSION=8.7.0

# --------------------------------------------------------------------------
# base: the same toolchain as the sandbox image (sandbox/image/Dockerfile);
# keep the two in step. A participant's tools compile against the same
# compiler their submission is built with.
# --------------------------------------------------------------------------
FROM ubuntu:24.04 AS base

ENV DEBIAN_FRONTEND=noninteractive \
    LANG=C.UTF-8

RUN apt-get update \
 && apt-get install -y --no-install-recommends \
      ca-certificates \
      curl \
      g++ \
      git \
      patch \
      python3 \
      unzip \
      zlib1g-dev \
 && rm -rf /var/lib/apt/lists/*

ARG BAZEL_VERSION
RUN curl -fsSL --retry 3 --retry-delay 2 -o /usr/local/bin/bazel \
      "https://github.com/bazelbuild/bazel/releases/download/${BAZEL_VERSION}/bazel-${BAZEL_VERSION}-linux-x86_64" \
 && chmod 0755 /usr/local/bin/bazel \
 && touch /etc/bazel.bazelrc

# --------------------------------------------------------------------------
# game_arena: an optional copy of a locally overridden game_arena module.
#
# Empty unless the tool passes `--build-context game_arena=<path>`, which it
# does when the kit's MODULE.bazel (or .bazelrc.local) points game_arena at a
# checkout on the host: that path does not exist in here. A problem pinned to
# a git commit needs none of this; bazel fetches the pin at build time.
# --------------------------------------------------------------------------
FROM scratch AS game_arena

# --------------------------------------------------------------------------
# The image: the toolchain, the kit, and its build.
# --------------------------------------------------------------------------
FROM base

COPY --from=game_arena --chown=ubuntu:ubuntu / /opt/arena/src/game_arena
RUN if [ -f /opt/arena/src/game_arena/MODULE.bazel ]; then \
      echo 'common --override_module=game_arena=/opt/arena/src/game_arena' \
        >> /etc/bazel.bazelrc; \
    fi

# ubuntu:24.04 ships a user "ubuntu" (uid 1000); the kit is theirs.
COPY --chown=ubuntu:ubuntu . /kit
USER ubuntu
WORKDIR /kit

# mcp.json names the kit's directory on the host it was written on; in here
# the kit is /kit.
RUN python3 -c 'import json; p = "mcp.json"; d = json.load(open(p)); \
      d["mcpServers"]["arena"]["cwd"] = "/kit"; \
      json.dump(d, open(p, "w"), indent=2); open(p, "a").write("\n")'

# Build everything once. Bazel's output base lands under ~/.cache/bazel, keyed
# by the workspace path, so a `bazel run` at run time finds it warm.
RUN bazel build //... && bazel shutdown

ARG ARENA_SERVER
ARG ARENA_HTTP
ARG ARENA_TOKEN
ARG ARENA_CLIENT_ID
# arena_cli reads the first two; the MCP server reads them too when its own
# ARENA_MCP_TARGET/ARENA_MCP_TOKEN are unset (mcp.json sets those explicitly).
ENV ARENA_SERVER=${ARENA_SERVER} \
    ARENA_TOKEN=${ARENA_TOKEN} \
    ARENA_MCP_AUTHOR=${ARENA_CLIENT_ID}
LABEL org.opencontainers.image.title="arena kit for ${ARENA_CLIENT_ID}" \
      org.opencontainers.image.description="Development environment for one participant of an arena tournament at ${ARENA_SERVER} (leaderboard: http://${ARENA_HTTP}/)"

CMD ["bash"]
