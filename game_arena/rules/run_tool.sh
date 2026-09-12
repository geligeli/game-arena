#!/bin/bash
# Runs the arena_tournament tool from a consumer's runfiles.
#
# arena_problem() generates sh_binary/sh_test targets whose srcs is this file
# and whose first argument is the tool's rootpath. Everything else is passed
# through. The tool finds problem_server, sandbox_worker and the sandbox
# Dockerfile through the runfiles library, which needs RUNFILES_DIR: `bazel
# run` and `bazel test` set it in different ways, so both are covered here.
set -euo pipefail

if [[ -z "${RUNFILES_DIR:-}" ]]; then
  if [[ -n "${TEST_SRCDIR:-}" ]]; then
    RUNFILES_DIR="${TEST_SRCDIR}"
  elif [[ -d "$0.runfiles" ]]; then
    RUNFILES_DIR="$0.runfiles"
  fi
  export RUNFILES_DIR
fi

TOOL="$1"
shift
exec "${TOOL}" "$@"
