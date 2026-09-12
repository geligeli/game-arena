# syntax=docker/dockerfile:1.7
#
# The three images a problem repository produces, one build target each. All
# are built by arena_tournament, never by hand, and share the `base` stage so
# a participant's tools, the sandbox and the tournament agree on a compiler.
#
#   --target sandbox     `arena_tournament image` / `bazel run //:sandbox_image`
#       The image sandbox.image names: the toolchain and every external
#       dependency the repo resolves, vendored, so workers build and run
#       submissions in it with no network. Context: the problem repo.
#
#   --target kit         `arena_tournament kit --image=TAG`
#       One participant's development environment: the kit the subcommand
#       wrote, the toolchain, and a completed build. Context: the kit dir.
#
#   --target tournament  `arena_tournament up --image=TAG`
#       The coordinator and its workers, ready to `docker run` on any host with
#       a docker socket: the arena's binaries built from the problem's
#       workspace, the problem repo for workers to clone, and no toolchain --
#       submissions build in the sandbox image, through the socket. Context:
#       the problem repo.
#
# Named build contexts (BuildKit, the default since Docker 23):
#   game_arena   optional. A copy of a locally overridden game_arena module,
#                passed by the tool when MODULE.bazel or .bazelrc.local points
#                game_arena at a checkout on the host, since that path does not
#                exist inside a build. A problem pinned to a git commit needs
#                none of this; bazel fetches the pin.
#   arena_image  this file's directory, for the tournament image to carry it
#                and build kit images from inside.
#
# Build args:
#   BAZEL_VERSION   the bazel release to install; the tool reads .bazelversion
#   kit:            ARENA_SERVER, ARENA_HTTP, ARENA_TOKEN, ARENA_CLIENT_ID --
#                   what the tool baked into arena.env and mcp.json
#   tournament:     PROBLEM_ID, PROBLEM_CONFIG (repo-relative), ARENA_KIT_FILES,
#                   ARENA_KIT_REGISTRY -- what `up` and `kit` need inside;
#                   DOCKER_VERSION and BUILDX_VERSION, the docker CLI and its
#                   build plugin to install
#
# A problem's committed .bazelrc must not name host paths (a shared cache on an
# NFS mount, say): it is read inside every one of these. Host-specific settings
# belong in an uncommitted .bazelrc.local, which no stage keeps.

ARG BAZEL_VERSION=8.7.0
ARG DOCKER_VERSION=29.8.0
ARG BUILDX_VERSION=0.37.1

# --------------------------------------------------------------------------
# base: the toolchain. Deliberately small -- no editors, no agent CLIs.
# --------------------------------------------------------------------------
FROM ubuntu:24.04 AS base

ENV DEBIAN_FRONTEND=noninteractive \
    LANG=C.UTF-8

# g++: bazel's default C++ toolchain shells out to /usr/bin/gcc.
# patch: bazel applies single_version_override patches with it.
# python3: rules_python's bootstrap, and problems' own scripts.
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
 && chmod 0755 /usr/local/bin/bazel

# ubuntu:24.04 ships a user "ubuntu" (uid 1000), the usual run_as_user. Both
# the vendored deps and the pre-extracted install base live under /opt/arena,
# readable by everyone: which uid the sandbox runs as is the problem's choice.
RUN mkdir -p /opt/arena && chown ubuntu:ubuntu /opt/arena \
 && touch /etc/bazel.bazelrc

# --------------------------------------------------------------------------
# game_arena: the optional copy of a locally overridden game_arena module (see
# the header). Stays empty unless the tool passes the build context.
# --------------------------------------------------------------------------
FROM scratch AS game_arena

# with_arena: base plus that copy, and a system bazelrc that uses it if there
# is one. Every stage that runs bazel starts here.
FROM base AS with_arena

COPY --from=game_arena --chown=ubuntu:ubuntu / /opt/arena/src/game_arena
RUN if [ -f /opt/arena/src/game_arena/MODULE.bazel ]; then \
      echo 'common --override_module=game_arena=/opt/arena/src/game_arena' \
        >> /etc/bazel.bazelrc; \
    fi

# ==========================================================================
# sandbox
# ==========================================================================

# --------------------------------------------------------------------------
# vendor: resolve and copy every external repository the problem needs.
# `bazel vendor` copies each into /opt/arena/vendor, and the sandbox's system
# bazelrc points bazel there at run time. A dependency the problem did not
# vendor is then a build failure, which is the correct outcome for a
# submission depending on something the problem did not offer.
# --------------------------------------------------------------------------
FROM with_arena AS vendor

COPY --chown=ubuntu:ubuntu . /src
USER ubuntu
WORKDIR /src
# The caches are switched off on the command line: a host's are in
# .bazelrc.local, which is not here, and none should be born here.
# --output_user_root off /src so nothing bazel writes lands in the tree that
# is about to be discarded: the vendor directory is the only thing kept.
RUN rm -f /src/.bazelrc.local \
 && bazel --output_user_root=/tmp/bazel_vendor \
      vendor --disk_cache= --repository_cache= \
      --vendor_dir=/opt/arena/vendor //... \
 && bazel --output_user_root=/tmp/bazel_vendor shutdown \
 && rm -rf /tmp/bazel_vendor

# --------------------------------------------------------------------------
# sandbox: the toolchain, the vendored deps, and a bazelrc that uses them.
# --------------------------------------------------------------------------
FROM base AS sandbox

COPY --from=vendor /etc/bazel.bazelrc /etc/bazel.bazelrc
COPY --from=vendor /opt/arena /opt/arena
# Readable by whoever the sandbox runs as. That is the problem's choice, and
# even root there has dropped every capability, so a file bazel vendored with
# owner-only permissions is a file the build cannot read.
RUN chmod -R a+rX /opt/arena

# System rc rather than the problem's bazel_flags: a worker's bazel invocation
# is the problem's business, and this is the image's. Bazel's own installation
# is not unpacked here: it wants a lock file beside itself, and the sandbox's
# root filesystem is read-only, so the worker points --install_base into the
# persistent output-base volume instead and pays for the unpack once per slot.
RUN echo 'common --vendor_dir=/opt/arena/vendor' >> /etc/bazel.bazelrc

# Vendor mode keeps a `bazel-external` symlink inside the vendor directory,
# pointing at the output base's external tree, and refuses to run if it cannot
# create it -- which on a read-only root it cannot. The worker mounts every
# sandbox's output base at /output_base (sandbox/common/docker.h), so the link
# can be made here once, with the target it will have at run time. The one
# `bazel vendor` left behind points into the build stage's scratch and is
# replaced.
RUN ln -sfn /output_base/external /opt/arena/vendor/bazel-external

# The image's default user stays root: who a sandbox runs as is the problem's
# choice (sandbox.run_as_user), and the tree it is handed is chowned to that
# user by the worker before anything starts.
USER root
WORKDIR /

# ==========================================================================
# kit: one participant's environment. Not a sandbox: it has whatever network
# and access `docker run` gives it, and only what they submit runs sandboxed.
#
#   docker run -it TAG                                  a shell in /kit
#   docker run -i TAG bazel run //:mcp_server           an MCP server on stdio
#   docker run -it TAG bazel run //:arena_cli -- leaderboard
#
# The arena's address and the token are baked in as environment, so the tools
# work without sourcing arena.env, and `docker run -e ARENA_SERVER=...` moves
# the kit to another coordinator.
# ==========================================================================
FROM with_arena AS kit

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

# ==========================================================================
# tournament: the coordinator and its workers.
#
#   docker run -d --name NAME -p 50051:50051 -p 8090:8090 \
#       -v /var/run/docker.sock:/var/run/docker.sock -v NAME:/var/arena TAG
#
# `arena_tournament up` inside runs problem_server and the workers, exactly as
# `bazel run //:tournament` does on a host. Workers clone the problem from
# /opt/arena/problem and run every build and match in the sandbox image
# through the mounted socket, so that image must be on the same daemon (`bazel
# run //:sandbox_image`, or a pull). State -- submissions, ratings, the client
# registry, the workers' checkouts -- lives in /var/arena; keep it a volume.
#
#   docker exec NAME arena_tournament kit --mint=bob --server=HOST:50051 \
#       [--image=REG/kit-bob --push]           a participant's kit, from inside
#   docker kill -s HUP NAME                    reload the client registry
#   docker run TAG sandbox_worker --server=HOST:50051
#                                              one more worker, anywhere
#
# Runs as root: the socket is root's, and the worker's containers drop every
# capability regardless. There is no bazel in here on purpose; a `--no_container`
# run needs a host, not this image.
# ==========================================================================

# --------------------------------------------------------------------------
# tournament_build: the arena's binaries, built from the problem's workspace
# so they are the arena the problem depends on. The cache mount keeps bazel's
# output across builds on one daemon; only the binaries leave this stage.
# --------------------------------------------------------------------------
FROM with_arena AS tournament_build

COPY . /src
WORKDIR /src
ARG PROBLEM_ID
RUN --mount=type=cache,target=/var/cache/bazel,sharing=locked \
    rm -f /src/.bazelrc.local \
 && B="bazel --output_user_root=/var/cache/bazel --output_base=/var/cache/bazel/tournament-${PROBLEM_ID}" \
 && T="@game_arena//game_arena/server:problem_server \
       @game_arena//game_arena/sandbox/worker:sandbox_worker \
       @game_arena//game_arena/tools:arena_tournament \
       @game_arena//game_arena/tools:arena_admin \
       @game_arena//game_arena/tools:arena_cli" \
 && $B build --disk_cache=/var/cache/bazel/disk \
      --repository_cache=/var/cache/bazel/repo $T \
 && FILES=$($B cquery --output=files "set($T)") \
 && for f in $FILES; do \
      install -D "$f" "/out/$(echo "$f" | sed -E 's#.*/external/game_arena[+~]/##')" || exit 1; \
    done \
 && test -x /out/game_arena/server/problem_server \
 && $B shutdown \
 && mkdir -p /opt/arena/src/game_arena

# --------------------------------------------------------------------------
# tournament: no toolchain. git for the workers' clones, the docker CLI for
# their containers, and the binaries laid out as in the arena's tree under
# /opt/arena, which is what ARENA_HOME tells arena_tournament.
# --------------------------------------------------------------------------
FROM ubuntu:24.04 AS tournament

ENV DEBIAN_FRONTEND=noninteractive \
    LANG=C.UTF-8

RUN apt-get update \
 && apt-get install -y --no-install-recommends \
      ca-certificates \
      curl \
      git \
      libstdc++6 \
 && rm -rf /var/lib/apt/lists/*

# The CLI for the workers' containers, and buildx so `kit --image` in here can
# build with BuildKit (the static CLI alone falls back to the legacy builder,
# which has no named build contexts).
ARG DOCKER_VERSION
ARG BUILDX_VERSION
RUN curl -fsSL --retry 3 --retry-delay 2 \
      "https://download.docker.com/linux/static/stable/x86_64/docker-${DOCKER_VERSION}.tgz" \
    | tar -xz -C /usr/local/bin --strip-components=1 docker/docker \
 && mkdir -p /usr/local/lib/docker/cli-plugins \
 && curl -fsSL --retry 3 --retry-delay 2 \
      -o /usr/local/lib/docker/cli-plugins/docker-buildx \
      "https://github.com/docker/buildx/releases/download/v${BUILDX_VERSION}/buildx-v${BUILDX_VERSION}.linux-amd64" \
 && chmod 0755 /usr/local/lib/docker/cli-plugins/docker-buildx

COPY --from=tournament_build /out/ /opt/arena/
COPY --from=arena_image Dockerfile /opt/arena/game_arena/image/Dockerfile
# The local game_arena copy, if the build had one, so a kit image built from
# in here (`kit --arena_override=/opt/arena/src/game_arena`) can find it.
COPY --from=tournament_build /opt/arena/src/game_arena /opt/arena/src/game_arena
RUN ln -s /opt/arena/game_arena/tools/arena_tournament \
          /opt/arena/game_arena/tools/arena_admin \
          /opt/arena/game_arena/tools/arena_cli \
          /opt/arena/game_arena/server/problem_server \
          /opt/arena/game_arena/sandbox/worker/sandbox_worker \
          /usr/local/bin/

# The problem, as the workers clone it. Its .git is what matters; the tree
# beside it is what `up` reads the config from.
COPY . /opt/arena/problem
WORKDIR /opt/arena/problem

ARG PROBLEM_ID
ARG PROBLEM_CONFIG=problem.textproto
ARG ARENA_KIT_FILES
ARG ARENA_KIT_REGISTRY
# ARENA_VOLUME_PREFIX: a container's hostname is its id, so the default
# (arena-<hostname>) would name fresh cache volumes every restart.
ENV ARENA_HOME=/opt/arena \
    ARENA_STATE_DIR=/var/arena \
    ARENA_PROBLEM_CONFIG=${PROBLEM_CONFIG} \
    ARENA_VOLUME_PREFIX=arena-${PROBLEM_ID} \
    ARENA_KIT_FILES=${ARENA_KIT_FILES} \
    ARENA_KIT_REGISTRY=${ARENA_KIT_REGISTRY}
LABEL org.opencontainers.image.title="arena tournament: ${PROBLEM_ID}" \
      org.opencontainers.image.description="Coordinator and sandbox workers for the ${PROBLEM_ID} tournament; needs the docker socket and the problem's sandbox image on that daemon"
VOLUME /var/arena
EXPOSE 50051 8090

CMD ["arena_tournament", "up"]
