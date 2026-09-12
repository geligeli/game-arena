# AGENTS.md

## What this repo is

The arena: a framework for running competitive programming problems. Agents
submit solutions, the arena builds them in a sandbox, runs them, and ranks them.
C++23, Bazel 8.x (Bzlmod, see `MODULE.bazel`). Depends on nothing outside the
BCR.

Read `README.md` first, then `game_arena/ARENA.md` (the submission loop) and
`game_arena/README.md` (the broker protocol).

```
game_arena/proto/      wire protocols; the coordinator's contract
game_arena/server/     coordinator: submissions, scheduling, standings, HTTP
game_arena/sandbox/    exec/ (the execution engine), common/ (docker mechanics),
                       worker/ (the fleet's own policy), runner/ (dev tool)
game_arena/referee/    match loop + broker protocol; entry points as libraries
game_arena/client/     the generic reference client
game_arena/testgame/   Nim: the arena's own game and reference registry
game_arena/problems/   nim.textproto
game_arena/tools/      arena_admin
game_arena/common/     subprocess wrapper
```

## The rule that matters

**The arena must not know what a problem is made of.** No `#include`, no bazel
label, no hardcoded path, and *no string* here may name a particular problem's
code. Strings count: the two couplings that survived longest into the split
were a hardcoded candidate-harness label and a hardcoded dependency allowlist,
both invisible to the symbol-level test because they were strings.

- Rules reach the arena through `tournament_broker::GameRegistry()`, which
  `referee/game_registry.h` **declares and never defines**. A referee, broker
  or client binary is "a registry + an entry-point library"
  (`referee:referee_main`, `referee:broker_server_main`,
  `client:random_client_main`). Registry libraries need `alwayslink = 1`:
  nothing depends on them by label.
- `referee/game_session.h` is the contract — serialized states and actions as
  byte strings, nothing else. An adapter for some game framework belongs with
  that framework, not here.
- Anything problem-specific belongs in the problem's `.textproto`:
  `match.referee_target`, `submission.harness`,
  `submission.allowed_dep_prefixes`.
- `server:no_problem_code_test` nm-scans `problem_server` for `GameRegistry`,
  `GameSession` and `arena_testgame::`. It must keep passing.
- Use `game_arena/testgame` (Nim) for arena-side coverage. If a test needs a
  real game to be meaningful, it belongs in a consumer repo, not here.

## The other seam

`game_arena/sandbox/exec` runs things in a sandbox and must not learn what the
sandbox is for. No order, candidate, submission, game, referee, ELO or metric
may enter it -- and no bazel semantics beyond argv that happens to start with
"bazel". It keeps its own job description (`sandbox_job.proto`) beside itself
rather than in `proto/`, so the property is checkable by reading one BUILD
file:

```sh
bazel query 'somepath(//game_arena/sandbox/exec/..., //game_arena/proto/...)'
bazel query 'somepath(//game_arena/sandbox/exec/..., //game_arena/server/...)'
bazel query 'somepath(//game_arena/sandbox/exec/..., //game_arena/sandbox/worker/...)'
```

All three must stay empty. The arena's own answer to "what is an order" lives
above it, in `sandbox/worker/order_job.h` (an order becomes a job),
`order_outcome.h` (a result becomes an outcome) and `order_runner.h` (the
gates), written once for both engines.

Isolation is one function, `exec/isolation.h`, and no path through the
container engine builds a `docker run` without it. That is what keeps the
claims in `ARENA.md` true of the dev runner as well as the fleet; before it,
the hardening was a private method of one backend and the runner was the
un-hardened counterexample.

## Build / test / run

```sh
bazel build //...
bazel test //...
bazel test --config=asan //game_arena/...
```

- Sanitizer / tuning configs in `.bazelrc` (each gets its own output dir):
  `--config=asan`, `--config=tsan`, `--config=ubsan`, `--config=msan`,
  `--config=native`.
- Headers are included with the full repo-relative path, e.g.
  `#include "game_arena/referee/game_session.h"`.
- The MCP server in `mcp_servers/arena_mcp` needs the arena running;
  see `mcp_servers/README.md`.

## C++ conventions

- C++23, `-Wall -Wextra -Werror` for project files; third-party under
  `external/` is silenced with `-w`, never "fix" warnings there.
- Format: Google style, `clang-format -i -style=google` (pre-commit hook).
- Header guards, never `#pragma once`. Guard form is repo name + path:
  `GAME_ARENA_GAME_ARENA_REFEREE_GAME_SESSION_H`. `scripts/fix_guards.py`
  (pre-commit) rewrites `#pragma once` and fails the commit so you re-stage.
- Loads come from `@rules_cc//cc:cc_library.bzl` etc., not bare `cc_library`.
- `package(default_visibility = ["//visibility:public"])` in BUILD files.

## Tests

- googletest via `@googletest//:gtest_main`, one `<name>_test.cc` + `cc_test`
  per unit.
- The sandbox integration tests assert exact docker argv against fake
  docker/git scripts, with no daemon. If one fails, behaviour changed — find
  out what before touching the expectation.
- Verify with `bazel test //...` plus `--config=asan` when touching the
  sandbox, the worker pool, or anything threaded.

## Do not

- Do not let a game, a problem, or a consumer repo's labels into
  `server/`, `sandbox/`, `proto/` or `referee/` — as code or as a string.
- Do not bump proto field numbers or "fix" them: `proto/` is the contract with
  every deployed worker and client. That rule is about the contract, not the
  directory: `sandbox/exec/sandbox_job.proto` and
  `sandbox/runner/sandbox_service.proto` pass between libraries in one process
  and are free to change.
- Do not use `#pragma once`, do not hand-write a guard without the
  `GAME_ARENA_` prefix, do not commit unformatted code (pre-commit handles it).
- Do not commit `bazel-*` symlinks/outputs, `compile_commands.json`, `.cache`,
  venvs, or generated proto artifacts (all git-ignored).
