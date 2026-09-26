# syntax=docker/dockerfile:1.7
#
# A kit base with coding agents: the arena's base (docker/base) plus Claude
# Code, opencode and the Antigravity CLI, each by its own installer. Built by
# hand like the base; a problem that wants it pulls it by digest and names it
# as arena_problem(kit_base = ...), in place of game_arena/image:kit_base.
#
#   docker build --provenance=false --sbom=false --push \
#       -t registry.takumi.city/game-arena-kit-agents:latest docker/coding-agents
#
# The installers fetch their latest releases, so a rebuild is a different
# image; the digest a problem pins is what stays put. No credentials in here:
# `docker run -e CLAUDE_CODE_OAUTH_TOKEN=...` and the like.

FROM registry.takumi.city/game-arena-base@sha256:52a3d7317cc865f1071e857506f03e42238d2bcb4db098f3c13f359440b1648f

# As the kit's user, so each agent lands in, and updates itself in, a home it
# owns.
USER ubuntu
WORKDIR /home/ubuntu
RUN curl -fsSL https://claude.ai/install.sh | bash
RUN curl -fsSL https://opencode.ai/install | bash
RUN curl -fsSL https://antigravity.google/cli/install.sh | bash

# Each agent's config, in the file that agent reads (home/): its permissions
# and model, and the arena's MCP server -- the kit's own arena_cli, run as
# `arena_cli mcp`. Claude Code takes that from the kit's .mcp.json. Nothing of
# a participant: the address, name and token are `docker run -e ARENA_*`.
COPY --chown=ubuntu:ubuntu home/ /home/ubuntu/

# What game_arena/image:kit_base sets on the base, which this image replaces,
# with the agents' directories on PATH.
ENV ARENA_KIT=/kit \
    PATH=/kit/.arena/bin:/home/ubuntu/.local/bin:/home/ubuntu/.opencode/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
WORKDIR /kit
