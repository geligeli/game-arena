# syntax=docker/dockerfile:1.7
# Needs BuildKit for the cache mounts: downloads live in build caches, not layers.

FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive \
    LANG=en_US.UTF-8 \
    PIP_NO_CACHE_DIR=1 \
    PYTHONDONTWRITEBYTECODE=1

# docker-clean would empty the package cache after each install, defeating the cache mount.
RUN rm -f /etc/apt/apt.conf.d/docker-clean \
 && printf 'Binary::apt::APT::Keep-Downloaded-Packages "true";\n' \
      > /etc/apt/apt.conf.d/keep-cache

# No compiler: one here could only hide a build that is not hermetic (the toolchain is
# hermetic-llvm). python3 is for fix_guards, new_problem.sh and process_test. The docker
# CLI comes from devcontainer.json's docker-outside-of-docker feature.
RUN --mount=type=cache,target=/var/cache/apt,sharing=locked \
    --mount=type=cache,target=/var/lib/apt/lists,sharing=locked \
    apt-get update \
 && apt-get install -y --no-install-recommends \
      ca-certificates \
      clang-format \
      clangd \
      curl \
      gdb \
      git \
      gnupg \
      graphviz \
      less \
      linux-tools-common \
      linux-tools-generic \
      locales \
      openssh-client \
      patch \
      python3 \
      python3-pip \
      ripgrep \
      sudo \
      unzip \
 && localedef -i en_US -c -f UTF-8 -A /usr/share/locale/locale.alias en_US.UTF-8

# Declared below the apt layer: an ARG is in the cache key of every RUN in its scope.
ARG BAZEL_VERSION=8.7.0
ARG BUILDIFIER_VERSION=8.2.1

# curl -f, so an HTTP error fails the build instead of installing an error page.
RUN --mount=type=cache,target=/var/cache/download,sharing=locked \
    set -eux; \
    dl() { \
      [ -s "/var/cache/download/$1" ] \
        || curl -fsSL --retry 3 --retry-delay 2 -o "/var/cache/download/$1" "$2"; \
    }; \
    dl "bazel-${BAZEL_VERSION}" \
      "https://github.com/bazelbuild/bazel/releases/download/${BAZEL_VERSION}/bazel-${BAZEL_VERSION}-linux-x86_64"; \
    dl "buildifier-${BUILDIFIER_VERSION}" \
      "https://github.com/bazelbuild/buildtools/releases/download/v${BUILDIFIER_VERSION}/buildifier-linux-amd64"; \
    install -m 0755 "/var/cache/download/bazel-${BAZEL_VERSION}" /usr/local/bin/bazel; \
    install -m 0755 "/var/cache/download/buildifier-${BUILDIFIER_VERSION}" /usr/local/bin/buildifier

RUN adduser ubuntu sudo \
 && echo '%sudo ALL=(ALL) NOPASSWD:ALL' > /etc/sudoers.d/nopasswd \
 && chmod 0440 /etc/sudoers.d/nopasswd

USER ubuntu
WORKDIR /home/ubuntu
ENV PATH="/home/ubuntu/.local/bin:/home/ubuntu/.kimi-code/bin:/home/ubuntu/.opencode/bin:${PATH}"

# The install base bazel unpacks into ~/.cache is shadowed by devcontainer.json's mount.
RUN set -eux; \
    bazel help completion bash > /home/ubuntu/.bazel-complete.bash; \
    echo 'source /home/ubuntu/.bazel-complete.bash' >> /home/ubuntu/.bashrc; \
    bazel shutdown || true; \
    rm -rf /home/ubuntu/.cache

# Agent CLIs last and one RUN each: they install whatever is current.
RUN curl -fsSL https://claude.ai/install.sh | bash
RUN curl -fsSL https://code.kimi.com/kimi-code/install.sh | bash
RUN curl -fsSL https://opencode.ai/install | bash

COPY --chown=ubuntu:ubuntu opencode.json /home/ubuntu/.config/opencode/opencode.json
