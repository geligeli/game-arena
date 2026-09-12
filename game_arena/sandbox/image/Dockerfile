# syntax=docker/dockerfile:1.7
#
# The sandbox image for one problem repository.
#
# Built by `arena_tournament image` with the problem repo as the build context,
# and tagged as the problem's sandbox.image. Workers run every build and every
# match inside it with no network, so the image must carry two things: bazel
# with a C++ toolchain, and every external dependency the repo's MODULE.bazel
# resolves to. The second is `bazel vendor`: it copies each repository into
# /opt/arena/vendor at image build time, and a system bazelrc points bazel at
# that directory at run time. A dependency the problem did not vendor is then a
# build failure, which is the correct outcome for a submission depending on
# something the problem did not offer.
#
# The problem's committed .bazelrc must not name host paths (a shared cache on
# an NFS mount, say): it is read inside the sandbox too, where they do not
# exist. Host-specific settings belong in an uncommitted .bazelrc.local.
#
# Needs BuildKit (the default since Docker 23) for the named build context
# below. Build args:
#   BAZEL_VERSION   the bazel release to install; the tool reads .bazelversion

ARG BAZEL_VERSION=8.7.0

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
# game_arena: an optional copy of a locally overridden game_arena module.
#
# Empty unless the tool passes `--build-context game_arena=<path>`, which it
# does when the problem's MODULE.bazel (or .bazelrc.local) points game_arena at
# a local checkout: `bazel vendor` symlinks local overrides rather than copying
# them, so the checkout has to be inside the image for the build to work
# without a network. A problem pinned to a git commit needs none of this.
# --------------------------------------------------------------------------
FROM scratch AS game_arena

# --------------------------------------------------------------------------
# vendor: resolve and copy every external repository the problem needs.
# --------------------------------------------------------------------------
FROM base AS vendor

COPY --from=game_arena --chown=ubuntu:ubuntu / /opt/arena/src/game_arena
RUN if [ -f /opt/arena/src/game_arena/MODULE.bazel ]; then \
      echo 'common --override_module=game_arena=/opt/arena/src/game_arena' \
        >> /etc/bazel.bazelrc; \
    fi

COPY --chown=ubuntu:ubuntu . /src
USER ubuntu
WORKDIR /src
# .bazelrc.local is the host's uncommitted business (a local game_arena
# override, a shared cache) and none of it exists in here; the caches are
# switched off on the command line for the same reason. --output_user_root
# off /src so nothing bazel writes lands in the tree that is about to be
# discarded: the vendor directory is the only thing kept.
RUN rm -f /src/.bazelrc.local \
 && bazel --output_user_root=/tmp/bazel_vendor \
      vendor --disk_cache= --repository_cache= \
      --vendor_dir=/opt/arena/vendor //... \
 && bazel --output_user_root=/tmp/bazel_vendor shutdown \
 && rm -rf /tmp/bazel_vendor

# --------------------------------------------------------------------------
# The image: the toolchain, the vendored deps, and a bazelrc that uses them.
# --------------------------------------------------------------------------
FROM base

COPY --from=vendor /etc/bazel.bazelrc /etc/bazel.bazelrc
COPY --from=vendor --chown=ubuntu:ubuntu /opt/arena /opt/arena

# System rc rather than the problem's bazel_flags: a worker's bazel invocation
# is the problem's business, and this is the image's. The install base is
# extracted here once instead of on every container start -- the sandbox's
# root filesystem is read-only and its HOME is scratch, so without this each
# order would begin by unpacking ~200MB of bazel into a tmpfs.
RUN echo 'common --vendor_dir=/opt/arena/vendor' >> /etc/bazel.bazelrc \
 && echo 'startup --install_base=/opt/arena/bazel_install' >> /etc/bazel.bazelrc
USER ubuntu
WORKDIR /
RUN bazel --output_user_root=/tmp/bazel_install version \
 && bazel --output_user_root=/tmp/bazel_install shutdown \
 && rm -rf /tmp/bazel_install

# The image's default user stays root: the sandbox's own isolation decides
# who runs (sandbox.run_as_user, or root for the in-container overlay, which
# needs it), and the read-only root filesystem it runs under makes a non-root
# default unable to create the mount points the entrypoint needs.
USER root
