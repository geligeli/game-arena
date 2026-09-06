# Splitting the arena out of game-mcts — status and remaining steps

The arena (`game_mcts/tournament_server/`) is being extracted into its own
repository. **Phase 1 is done and is what you are looking at**; phases 2 and 3
remain. This file replaces the original handoff instructions, which had the
dependency edge backwards — see "What changed and why" below.

## The shape

The arena is a general problem-running framework: a coordinator, a sandbox
fleet, and a referee harness. It hosts *problems*, of which "play Risk" is one
and "make this benchmark faster" is another. It must therefore depend on **no**
game code, and it now doesn't.

```
game_mcts/tournament_server/     the arena. Depends on nothing else in this repo.
  proto/        wire protocols (arena, sandbox_runner, tournament_broker, problem)
  server/       coordinator: submissions, scheduling, ELO/metric standings, HTTP
  sandbox/      common/ (docker mechanics), worker/ (fleet), runner/ (dev tool)
  referee/      match loop + broker protocol, entry points as linkable libraries
  client/       random_client_main: the generic reference client
  testgame/     Nim: the arena's own game, plus the reference registry
  problems/     nim.textproto, the reference problem
  tools/        arena_admin
  common/process/  -- lives at game_mcts/common/process until phase 2

game_mcts/arena/                 what binds THIS repo to the arena.
  game_session_impl.h   any mcts::SerializableGame as an arena GameSession
  builtins.h            random/minimax builtins over the mcts concepts
  game_registry.cc      the risk2 / tictactoe / bench registry
  BUILD                 match_referee, broker_server, random_client
  client/ candidate_api/ candidates/ benchgame/ problems/
```

### The seam

`referee/game_registry.h` **declares** `GameRegistry()` and defines it nowhere.
Binaries are assembled as *a registry plus a game-agnostic entry-point library*:

```python
cc_binary(
    name = "match_referee",
    deps = [":game_registry", "//game_mcts/tournament_server/referee:referee_main"],
)
```

`referee:referee_main`, `referee:broker_server_main` and
`client:random_client_main` are `cc_library(alwayslink = 1)` targets holding
`main()`. Registry libraries also need `alwayslink = 1`: nothing depends on them
by label, so the linker would otherwise skip the archive member.

Everything else problem-specific is configuration, not code:
`match.referee_target` names the referee to build, `submission.harness` carries
the labels a generated candidate BUILD is written against, and
`submission.allowed_dep_prefixes` the deps a solution may name.

## Verified facts (re-verified at each phase)

Proven by `bazel query`, not by reading:

```sh
# Empty. The arena depends on no game code.
bazel query 'filter("^//game_mcts/(core|games|arena)", deps(//game_mcts/tournament_server/...))'

# Empty. The coordinator links no registry.
bazel query 'filter("game_registry|testgame|^//game_mcts/arena", deps(//game_mcts/tournament_server/server:problem_server))'

# Only the arena's sandbox. common/process moves with the arena in phase 2.
bazel query 'rdeps(//game_mcts/tournament_server/... + //game_mcts/core/... + //game_mcts/games/... + //game_mcts/arena/..., //game_mcts/common/process, 1)'
```

`server:no_problem_code_test` nm-scans the linked `problem_server` for
`tournament_broker::GameRegistry`, `tournament_broker::GameSession` and
`arena_testgame::`. It names the registry and the session rather than a list of
games precisely because the arena cannot enumerate the games that exist.

## Phase 2 — extract into `game-arena`

`git filter-repo` is not installed on the host; `pip install --user
git-filter-repo` first. Run outside the devcontainer (it is not provisioned for
pushing to GitHub); bazel is available inside it via `docker exec`.

```sh
git clone /large_nfs/game-mcts game-arena && cd game-arena
git filter-repo --path game_mcts/tournament_server \
                --path game_mcts/common/process \
                --path mcp_servers/arena_mcp \
                --path scripts/fix_guards.py \
                --path .bazelrc --path .bazelversion --path .clang-format \
                --path .pre-commit-config.yaml --path .gitignore \
                --path BUILD \
                --path protobuf_python_dist_build.patch \
                --path protobuf_python_dist_bzl.patch
```

The empty root `BUILD` is needed so `//:protobuf_python_dist_*.patch` resolves
in `single_version_override`.

Then reprefix `game_mcts/tournament_server/<x>` -> `game_arena/<x>` and
`game_mcts/common/process` -> `game_arena/common/process`. Mostly includes,
labels and header guards, but three places encode the path as a *string*:

- `proto/BUILD` — `-Igame_mcts/tournament_server/proto` in `arena_grpc_py_gen`.
- `mcp_servers/arena_mcp/make_stubs.sh` and `server.py` — the
  `game_mcts.tournament_server.proto` import path.
- `server/no_problem_code_test.sh` — `BINARY=` is runfiles-relative.

Guards come from `scripts/fix_guards.py`: `basename(git_root) + "_" + rel_path`,
so sources under `game_arena/` in a repo cloned as `game-arena` get
`GAME_ARENA_GAME_ARENA_...`, matching this repo's own doubled convention.
Namespaces (`tournament_broker`, `tournament_arena`, `sandbox_common`,
`arena_testgame`) carry no repo name and do not change.

### New MODULE.bazel — no `game_mcts` dep

```python
module(name = "game_arena", version = "0.1.0")

bazel_dep(name = "platforms", version = "1.0.0")
bazel_dep(name = "rules_python", version = "1.6.3")   # arena_mcp + py_proto
bazel_dep(name = "rules_cc", version = "0.2.14")
bazel_dep(name = "rules_shell", version = "0.7.1")    # no_problem_code_test
bazel_dep(name = "googletest", version = "1.17.0")
bazel_dep(name = "re2", version = "2025-11-05.bcr.1") # sandbox/worker:build_log
bazel_dep(name = "abseil-cpp", version = "20250814.1")
bazel_dep(name = "protobuf", version = "32.1")
bazel_dep(name = "grpc", version = "1.74.1")
bazel_dep(name = "boringssl", version = "0.20241024.0")  # server/client_registry

single_version_override(
    module_name = "protobuf",
    patches = ["//:protobuf_python_dist_build.patch",
               "//:protobuf_python_dist_bzl.patch"],
    patch_strip = 1,
)

python = use_extension("@rules_python//python/extensions:python.bzl", "python")
python.toolchain(python_version = "3.12")
pip = use_extension("@rules_python//python/extensions:pip.bzl", "pip")
pip.parse(hub_name = "mcp_pip_deps", python_version = "3.12",
          requirements_lock = "//mcp_servers:requirements.txt")
use_repo(pip, "mcp_pip_deps")
```

No `local_path_override`, no sibling checkout. This matters operationally, not
just aesthetically: sandbox workers `git clone --local` the repo
(`sandbox/worker/checkout.cc`) and build it in a **closed-network** container
(see ARENA.md), where an override pointing at a host path would not resolve.

Do not copy `MODULE.bazel.lock`; regenerate and commit it.

Trim `mcp_servers/` to `arena_mcp`: drop `smoke_test` (it drives both servers),
keep the arena half of `README.md`, and keep `mcp` + `grpcio` in
`requirements.txt`.

### Verify, in order

1. `bazel build //game_arena/proto/...`
2. `bazel build //game_arena/common/process/... //game_arena/server:problem_server`
3. `bazel test //game_arena/sandbox/...` — fake docker/git scripts, no daemon.
4. `bazel build //game_arena/referee/... //game_arena/testgame/...`
5. `bazel build //... && bazel test //...` — 29 tests, `no_problem_code_test`
   included.
6. `bazel test --config=asan //...`

## Phase 3 — game-mcts consumes game-arena

1. `bazel_dep(name = "game_arena", version = "0.1.0")` plus a
   `local_path_override` to the sibling checkout, with the `git_override` form
   commented beside it for CI.
2. Delete `game_mcts/tournament_server/` and `game_mcts/common/process`, and
   the commented consumer-module sketch at the bottom of `MODULE.bazel`.
3. Repoint `game_mcts/arena/**/BUILD` and `game_mcts/tools/bench/BUILD` at
   `@game_arena//game_arena/...`, and the labels inside
   `game_mcts/arena/problems/*.textproto`.
4. `boringssl`, `re2` and `rules_shell` then have no users left in game-mcts
   (their only references are in the arena). `grpc` survives via
   `arena/client:remote_client` and `tools/bench:throughput_benchmark`. Drop
   the three in a separate commit so the removal stays mechanical.
5. `bazel build //... && bazel test //...`.

**Do not push or merge the game-mcts removal** — that is a review step.

## Guardrails

- Nothing under `game_mcts/tournament_server/` may name `core/`, `games/` or
  `arena/`, as an include, a label, or a **string**. The two couplings that
  survived the longest were strings: a hardcoded candidate-harness label in
  `server/generated_build.cc` and a hardcoded dependency allowlist in
  `server/candidate_store.cc`. Both are problem config now.
- The sandbox integration tests assert exact docker argv from fake-docker logs.
  If one fails, behaviour changed — stop and find it.
- Do not bump proto field numbers. The coordinator's contract is `proto/`.

## What changed and why

The original version of this document proposed moving the arena out **with a
dependency on `@game_mcts`**, on the grounds that `referee/`, `client/`,
`candidate_api/` and `candidates/` are game-coupled and had to travel with it.
They are game-coupled — but they are the *instantiation* of the arena for these
games, not the arena, and keeping them was the only thing creating the edge.

Two of that document's load-bearing facts were also wrong:

- "`common/process` is also used by `core/mcts`, so it stays in game-mcts." It
  wasn't. `core/mcts/BUILD` declared the dep on `mcts_test`, but no source
  under `core/`, `games/` or `common/` included `process.h`. The stale line is
  gone and the library moves with the arena.
- "`referee/` is the one place game code lives." Nine of its twelve libraries
  had no external includes at all. Only `game_session.h` (which bundled the
  abstract interface with the `mcts::` adapter), `builtins.h` and
  `game_registry.cc` were coupled.

## Follow-ups (not part of the split)

- `SetDefaultMctsIterations` in `referee/game_registry.h` names a search
  algorithm in an interface that should not know about one. Generalise it to an
  opaque per-registry options blob; every registry currently has to define it,
  and `testgame`'s is a no-op.
- Per-problem sandbox limits (memory/cpus/user) still come from worker flags;
  `SandboxSpec` defines them but only `require_container` rides on the
  `WorkOrder`.
- `arena/candidate_api` selects its game with `CANDIDATE_GAME_*` local_defines;
  a data-driven form would let one harness serve more problems.
