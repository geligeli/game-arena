#!/bin/bash
# Execs the tool ($1) with RUNFILES_DIR, which `bazel run` and `bazel test` set differently.
set -euo pipefail
set -x

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
