#!/bin/bash
# The arena a kit gets must be the whole arena a kit needs.
#
# `arena_tournament kit` vendors six packages into every kit (//:kit_surface).
# If something a kit builds grows a dependency outside them, the kit stops
# building -- and the failure lands on a participant, in a directory they
# cannot fix, long after the change that caused it. So the closure is checked
# here, where it is one line of output instead.
#
# The query itself runs at build time (:kit_surface_deps); this only reads it.
set -uo pipefail

deps="${1:-}"
if [[ ! -f "$deps" ]]; then
  echo "usage: $0 <kit_surface_deps>; the query output is the test's data" >&2
  exit 1
fi

# Every package the vendored copy carries. A dependency outside these is what
# this test exists to catch.
allowed='^//game_arena/(proto|referee|client|cli|common/kv_options|standings):'

outside="$(grep -E '^//' "$deps" | grep -Ev "$allowed" || true)"
if [[ -n "$outside" ]]; then
  echo "These are reachable from what a kit builds but are not in the kit" >&2
  echo "surface, so a kit would not build:" >&2
  echo "$outside" | sed 's/^/  /' >&2
  echo >&2
  echo "Either keep the dependency out, or add its package to //:kit_surface" >&2
  echo "and to kKitSurfaceDirs in game_arena/tools/arena_tournament.cc -- and" >&2
  echo "then ask whether a participant should be reading it." >&2
  exit 1
fi
echo "kit surface is closed: $(grep -cE '^//' "$deps") in-repo deps, all vendored"
