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
game_arena/server/     coordinator: submissions, scheduling, HTTP
game_arena/standings/  rating, history and the leaderboard page -- shared with
                       the local broker a participant runs, so it depends on
                       nothing in server/
game_arena/sandbox/    exec/ (the execution engine), common/ (docker mechanics),
                       worker/ (the fleet's own policy), runner/ (dev tool)
game_arena/referee/    match loop + broker protocol; entry points as libraries
game_arena/client/     the generic reference client
game_arena/testgame/   Nim: the arena's own game and reference registry
game_arena/problems/   nim.textproto
game_arena/rules/      arena_problem, the macro a problem repo calls
game_arena/image/      what bazel layers a problem's images onto: kit_base
                       and sandbox_base
docker/base/           the base image's Dockerfile: the one image docker
                       builds, by hand, pinned by digest in MODULE.bazel
game_arena/cli/        arena_cli: the participant's client, a kit's builtin
game_arena/tools/      arena_admin, arena_tournament (operator tooling)
game_arena/common/     subprocess wrapper
scripts/new_problem.sh scaffolds a problem repo from an example
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
- `rules/problem.bzl` and `tools/arena_tournament` are how a problem repo
  becomes a tournament. Every label, file and name they act on arrives from
  the caller: the macro's `registry` and `kit_files`, the config's targets.
  Nothing in either names a problem.

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

The container engine never bind-mounts anything of its own: the tree and the
patches are loaded into per-job volumes through the daemon and the caches
persist in named volumes, so a worker is "anything with a docker socket" and
no sandbox needs a capability. `Mount::BIND` exists only as a host's opt-in
for its caches. Do not add a bind mount to make something work.

There is no unsandboxed backend. `sandbox_worker` links the container engine
and nothing else, `sandbox.image` is required by `ValidateProblemConfig`, and
`exec:process_engine` is visible only to the two packages whose tests use it.
It exists to test the step kernel without a daemon and to give
`capabilities().isolates` a false case; do not link it into anything that
runs a submission, and do not add a flag that would.

## What a participant gets

A kit is the third thing this repo produces, after the coordinator and the
fleet, and it is the one with a person on the other end. Its rule: **only what
they need to work on the problem.**

- `//:kit_surface` is the arena a kit vendors -- `proto/`, `referee/`,
  `client/`, `cli/`, `standings/`, `common/kv_options/` and the MCP server.
  Each is closed under dependency and `//game_arena:kit_surface_test` fails
  when that stops being true. Adding a package to it is a decision about what
  a participant should be reading, not a build fix.
- `arena_cli` is a program in the kit, not a bazel target: entering a
  tournament must not require a toolchain. It reads the kit's
  `arena.textproto` (`proto/kit.proto`) for its defaults, which the
  participant may edit -- the coordinator enforces the problem's policy on
  whatever arrives, so a widened kit config changes what is sent, never what
  is accepted.
- A kit image is a build output: `bazel build //:kit_image` is the kit's tree
  (`rules/kit_tree.bzl`, this tool run in an action) stacked on
  `//game_arena/image:kit_base` by rules_oci. No container runs while it is
  made, and nothing goes into it that is not a build's to hold. The two
  things that are not -- priming, which is bazel run on a kit, and a token,
  which is a secret and would sit in a remote cache -- are **external
  actions**: `//:kit_image_issue` adds each as a layer on the built image, at
  run time. Do not move either into an action. Priming runs with
  `--nohome_rc --nosystem_rc` and a strict action env, because a cache hits
  only for the build that filled it.
- **A participant is a directory**, `<files_submit_dir>/<name>/`, the same in
  the problem's tree, at the coordinator and in a kit. The candidate id is
  the participant (the token's client id): one entry, one row, one rating,
  and a resubmit -- staged until it builds -- replaces the code behind them.
  The generated BUILD names no directory (`package_name()`), so a rival's
  pulled by `arena_cli source <name>` builds where it lands, and
  `arena_cli spar <name>` plays yours against it locally. That runs a rival's
  code on the participant's machine, by their choice, without their token; it
  is not a path the tournament runs anything on. There is no `Evaluate`:
  placement -- builtins, then a ladder of rated rivals -- is the only thing
  that spends the fleet.
- `arena_cli` spawns with `posix_spawnp`, not `common/process`: a kit's
  surface was not widened for `spar`.
- What of each other participants may read is `SourcePolicy` in the problem
  config, enforced in `arena_service.cc` on `GetSource` and on the patch bytes
  of every manifest. The default is that everything is readable; that is the
  point of the arena, and a problem opts out of it deliberately.

## Images

No image here is a docker build, except the base (`docker/base/Dockerfile`:
`apt-get` needs a container to run in, and nothing else does). The rest follow
one split, and it is the thing to keep:

- **What a build can hold is a build output.** rules_oci stacks tars on a base
  pulled by digest: `//:kit_image`, `//:sandbox_image`,
  and the arena's own `kit_base` and `sandbox_base`. Nothing runs inside an image while it is made.
- **What a build cannot hold is added outside one**, as more layers, by
  `arena_tournament` with the regctl rules_oci built the image with: a token
  and the result of running bazel -- a kit's vendored dependencies and primed
  cache -- both by `kit_image_issue`, and nothing else. Do not turn either
  into an action: a secret ends up in a remote cache, and bazel does not run
  bazel.
- **The sandbox image vendors nothing.** It is the base, the arena's sources
  (`//:sandbox_surface`, which its bazelrc overrides `game_arena` to) and the
  problem's tree. The first build in a slot fetches what the problem resolves
  into that slot's output-base volume, so a problem sets
  `sandbox.allow_build_network`, and `run_as_user` to someone who is not root
  (unpacking an archive as root restores owners, which a sandbox cannot).
- **The problem's tree is in its sandbox image**, at `/workspace`: the root
  package's files and the `tree` filegroups each other package exports, since
  a glob does not cross packages. Every job gets a fresh volume mounted there, and docker
  fills a fresh volume from what the image has at the mount point: that is
  how a worker gets the tree, and why it needs no git, no repository and no
  reset between jobs. There is no `repo.url` and no `base_commit`; the image
  tag is what two comparable submissions have in common. `up` from a checkout
  loads it every time, a cached build, so an edit is never missed.
- Every target that reaches the base is tagged `manual`. `//...` must not need
  a registry, and a sandbox's `bazel vendor //...` must not carry an image.
  `up` makes the sandbox image by *running* `//:sandbox_image_load`, for the
  same reason: carrying the base itself would put it in `//...`.
- `tournament` is the coordinator alone: it never starts a worker, makes an
  image or calls docker. A worker is a separate process, started by whoever
  wants the capacity (`deploy.sh`; `play` for the dev loop).
- The coordinator and the workers are not images. They are processes on a
  host with docker (`bazel run //:tournament`); only what a worker builds and
  runs is a container. Do not wrap them in one: it buys a socket mount and
  nothing else.

## Build / test / run

```sh
bazel build //...
bazel test //...
bazel test --config=asan //game_arena/...
(cd examples/connect4 && bazel test //...)   # the macro, from a consumer
(cd examples/knapsack && bazel test //...)
```

End to end, from an example as it sits (`scripts/new_problem.sh` is for
starting a problem of your own, not a prerequisite):

```sh
cd examples/connect4
bazel run //:play              # tournament + your kit + a shell in it
arena_cli submit --wait        # the kit's builtin; sends bots/<you>/
arena_cli spar reference       # yours against the starter, locally
exit                           # stops the tournament
```

`play` makes the sandbox image first if the daemon does not have it, which is
the slow part of a first run. Every submission is built and run in a
container; there is no flag that skips that.

Or the pieces `play` runs: `bazel run //:tournament` in one shell,
`bazel run //:kit -- --out=/tmp/kit --mint=alice` in another, then
`. ./arena.env` in the kit.

- Sanitizer / tuning configs in `.bazelrc` (each gets its own output dir):
  `--config=asan`, `--config=tsan`, `--config=ubsan`, `--config=native`.
  No msan: the toolchain has no msan-instrumented libc++ (see `.bazelrc`).
- Headers are included with the full repo-relative path, e.g.
  `#include "game_arena/referee/game_session.h"`.
- The MCP server in `mcp_servers/arena_mcp` needs the arena running;
  see `mcp_servers/README.md`.

## C++ conventions

- C++23, `-Wall -Wextra -Werror` for project files; third-party under
  `external/` is silenced with `-w`, never "fix" warnings there. A warning
  clang raises *inside* an external header included from project code is
  disabled by category in `.bazelrc`, not by editing the dependency.
- The compiler is the hermetic-llvm module in `MODULE.bazel` (clang, libc++,
  compiler-rt; no sysroot, nothing from the host). Registered by the arena so
  every consumer, kit and sandbox builds with the same one; that is why the
  sandbox image installs no compiler. Sanitizer configs use its
  `--@llvm//config:<san>=true` settings rather than raw `-fsanitize` flags.
- Format: Google style, `clang-format -i -style=google` (pre-commit hook).
- Header guards, never `#pragma once`. Guard form is repo name + path:
  `GAME_ARENA_GAME_ARENA_REFEREE_GAME_SESSION_H`. `scripts/fix_guards.py`
  (pre-commit) rewrites `#pragma once` and fails the commit so you re-stage.
- Loads come from `@rules_cc//cc:cc_library.bzl` etc., not bare `cc_library`.
- `package(default_visibility = ["//visibility:public"])` in BUILD files.

## Size

- Write the smallest thing that works. If 3 lines do it, write 3 lines.
- Happy path only. No argument validation, env-var knobs, retry/wait loops,
  fallbacks or usage text unless asked. `set -e` and the tool's own error are
  the error handling.
- Comments: one line, only where the code is surprising.
- If a change will exceed ~30 lines, say what and why before writing it.
  Offer hardening as a one-line follow-up, don't build it.

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
- Do not add a path that runs a submission outside a container, and do not
  link `exec:process_engine` into anything that runs one. The flag that used
  to do this (`--no_container`) is gone on purpose.
- Do not widen `//:kit_surface` to make a kit build. What a participant can
  read is a decision, not a dependency fix.
- Do not bump proto field numbers or "fix" them: `proto/` is the contract with
  every deployed worker and client. That rule is about the contract, not the
  directory: `sandbox/exec/sandbox_job.proto` and
  `sandbox/runner/sandbox_service.proto` pass between libraries in one process
  and are free to change.
- Do not use `#pragma once`, do not hand-write a guard without the
  `GAME_ARENA_` prefix, do not commit unformatted code (pre-commit handles it).
- Do not commit `bazel-*` symlinks/outputs, `compile_commands.json`, `.cache`,
  venvs, or generated proto artifacts (all git-ignored).
