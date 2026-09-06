#!/bin/bash
# The coordinator must link no game and no problem code.
#
# It is a coordinator because it stores submissions, schedules them and
# publishes standings -- not because a comment says so. The moment it links a
# game it can referee one, and the next person to need "just a quick validation"
# will. This is the check that makes that a build failure instead of a decision.
#
# Reads the linked binary's symbols rather than the dependency graph: there is
# more than one route to game code (the referee package, a game target directly,
# a library that grew a dep), and symbols do not care which was taken.

set -uo pipefail

BINARY="game_mcts/tournament_server/server/problem_server"
if [[ ! -x "${BINARY}" ]]; then
  echo "FAIL: ${BINARY} not found in runfiles" >&2
  exit 1
fi

if ! command -v nm >/dev/null 2>&1; then
  echo "SKIP: nm is not available, cannot inspect symbols" >&2
  exit 0
fi

# The door all problem code comes through. Naming the registry and the session
# rather than a list of games is the point: the arena does not know which games
# exist, so it cannot enumerate them -- but nothing can reach a game without
# one of these two. arena_testgame:: is this repo's own game, which has no more
# business in the coordinator than anyone else's.
FORBIDDEN='tournament_broker::GameRegistry|tournament_broker::GameSession|arena_testgame::'

FOUND="$(nm -C --defined-only "${BINARY}" 2>/dev/null | grep -E "${FORBIDDEN}" | head -20)"
if [[ -n "${FOUND}" ]]; then
  echo "FAIL: the coordinator links problem code. Offending symbols:" >&2
  echo "${FOUND}" >&2
  echo >&2
  echo "Game rules belong in a registry linked into the referee, which runs" >&2
  echo "on a sandbox worker. The coordinator only schedules and publishes." >&2
  exit 1
fi

echo "PASS: no problem code linked into ${BINARY}"
