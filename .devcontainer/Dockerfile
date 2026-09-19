# syntax=docker/dockerfile:1.7
#
# Needs BuildKit (the default since Docker 23) for the cache mounts below: apt's
# package lists and .debs, and the downloaded release binaries, live in build
# caches instead of in the image. A rebuild after a cache-busting edit
# re-downloads nothing, and none of it ships in the final layers.

FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive \
    LANG=en_US.UTF-8 \
    PIP_NO_CACHE_DIR=1 \
    PYTHONDONTWRITEBYTECODE=1

# The base image ships /etc/apt/apt.conf.d/docker-clean, which deletes the
# package cache after every install and so defeats a cache mount. Keep the
# downloads instead -- they are in a cache mount, not in the image.
RUN rm -f /etc/apt/apt.conf.d/docker-clean \
 && printf 'Binary::apt::APT::Keep-Downloaded-Packages "true";\n' \
      > /etc/apt/apt.conf.d/keep-cache

# One apt layer, --no-install-recommends, lists and .debs in cache mounts.
#
# No compiler: bazel's is the hermetic-llvm module in MODULE.bazel, and .bazelrc
# sets BAZEL_DO_NOT_DETECT_CPP_TOOLCHAIN so it never looks for a host one. A g++
# or clang here could only hide a build that is not hermetic. For the same
# reason there is no clang-tidy: it depends on a whole clang-18, and linting is
# cpp_format's job (tools/cpp_format.sh). What is left of LLVM is the two tools
# that run outside bazel: clang-format for the pre-commit hook, and clangd,
# which reads the hermetic toolchain's flags and headers out of the
# compile_commands.json that `tools/cpp_format.sh compile_commands` writes.
#
# --no-install-recommends means anything the tooling needs has to be named here
# explicitly. The non-obvious entries:
#   less, patch,   git recommendations the tooling actually relies on: pagers,
#   openssh-client bazel's patch fallback for single_version_override, and
#                  git-over-SSH.
#   python3        outside bazel only (the hermetic rules_python toolchain
#                  covers the build): pre-commit's fix_guards hook,
#                  scripts/new_problem.sh, and process_test, which spawns
#                  /usr/bin/python3.
#
# The docker CLI is deliberately absent: the docker-outside-of-docker feature in
# devcontainer.json installs it (plus buildx and compose), so installing
# docker-ce-cli here only duplicated it and added a second apt repo to set up.
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

# Pinned tool versions, declared here rather than at the top of the file: an ARG
# is part of the cache key of every RUN it is in scope for, so declaring these
# above the apt layer made a version bump re-run the apt install too.
ARG BAZEL_VERSION=8.7.0
ARG BUILDIFIER_VERSION=8.2.1

# Release binaries, one layer, downloads kept in a cache mount so a version bump
# only fetches the binary that changed. curl gets -f so an HTTP error fails the
# build instead of installing an HTML error page as an executable.
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

# Generating the completion script runs bazel, which extracts its ~195MB install
# base into ~/.cache. devcontainer.json bind-mounts the host's ~/.cache over that
# at runtime, so it was dead weight in the image -- drop it in the same layer.
RUN set -eux; \
    bazel help completion bash > /home/ubuntu/.bazel-complete.bash; \
    echo 'source /home/ubuntu/.bazel-complete.bash' >> /home/ubuntu/.bashrc; \
    bazel shutdown || true; \
    rm -rf /home/ubuntu/.cache

# Agent CLIs last: they install whatever is current, so they are the layers most
# likely to be rebuilt, and nothing below them would stay cached. One RUN each so
# re-running one does not re-download the other two.
RUN curl -fsSL https://claude.ai/install.sh | bash
RUN curl -fsSL https://code.kimi.com/kimi-code/install.sh | bash
RUN curl -fsSL https://opencode.ai/install | bash

COPY --chown=ubuntu:ubuntu opencode.json /home/ubuntu/.config/opencode/opencode.json
