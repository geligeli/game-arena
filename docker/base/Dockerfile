# syntax=docker/dockerfile:1.7
#
# The arena's base image: bazel and what it needs to run, and nothing of the
# arena or of any problem. It is the one image here that docker builds, by
# hand and rarely; every other -- a kit, the sandbox -- is
# layered on top of it by bazel (rules_oci), which pulls this by digest and
# never runs anything inside it. That split is the point: `apt-get` needs a
# container to run in, and nothing else an arena image is made of does.
#
#   docker build --provenance=false --sbom=false \
#       -t registry.takumi.city/game-arena-base:noble-bazel8.8.0-nano docker/base
#   docker push registry.takumi.city/game-arena-base:noble-bazel8.8.0-nano
#
# Then pin what the push printed in MODULE.bazel (oci.pull, name "arena_base").
# The tag is for people; the digest is what builds use, so a rebuild of this
# file changes nothing until that line changes. Without the two flags buildx
# attaches an attestation and pushes a manifest *list* -- the image plus an
# "unknown/unknown" entry -- and the digest to pin would be the list's.
#
# Deliberately small -- one editor for a participant in a kit, no agent CLIs,
# and no compiler: the C++
# toolchain is a bazel module the arena depends on (hermetic-llvm, see
# MODULE.bazel), fetched and vendored like any other dependency, so a kit, the
# sandbox and a developer's checkout compile with the same pinned clang
# whatever their distro ships.
#
# Build args:
#   BAZEL_VERSION   the bazel release to install. A kit's cache is primed on
#                   the host that writes it, so this should be the release
#                   that host runs.

FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive \
    LANG=C.UTF-8

# git: a participant's own, and bazel's for a git_override.
# patch: bazel applies single_version_override patches with it.
# python3: rules_python's bootstrap, and problems' own scripts.
# nano: something to edit a strategy with, in a kit's shell.
RUN apt-get update \
 && apt-get install -y --no-install-recommends \
      ca-certificates \
      curl \
      git \
      nano \
      patch \
      python3 \
      unzip \
 && rm -rf /var/lib/apt/lists/*

ARG BAZEL_VERSION=8.8.0
ARG TARGETARCH
RUN case "${TARGETARCH}" in \
      amd64) arch=x86_64 ;; \
      arm64) arch=arm64 ;; \
      *) echo "no bazel release for ${TARGETARCH}" >&2; exit 1 ;; \
    esac \
 && curl -fsSL --retry 3 --retry-delay 2 -o /usr/local/bin/bazel \
      "https://github.com/bazelbuild/bazel/releases/download/${BAZEL_VERSION}/bazel-${BAZEL_VERSION}-linux-${arch}" \
 && chmod 0755 /usr/local/bin/bazel

# ubuntu:24.04 ships a user "ubuntu" (uid 1000), the usual owner of a kit and
# the usual run_as_user of a sandbox. /opt/arena is where an image built on
# this one keeps what it adds. There is no compiler in this image, and bazel
# must not go looking for one: without this it autoconfigures a host toolchain
# repository it never uses and fails when there is nothing to find.
RUN mkdir -p /opt/arena && chown ubuntu:ubuntu /opt/arena \
 && echo 'common --repo_env=BAZEL_DO_NOT_DETECT_CPP_TOOLCHAIN=1' \
      > /etc/bazel.bazelrc

LABEL org.opencontainers.image.title="game-arena base" \
      org.opencontainers.image.description="ubuntu 24.04 with bazel ${BAZEL_VERSION}, git and python3; the base every arena image is layered on"

CMD ["bash"]
