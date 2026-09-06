# Splitting the arena out of game-mcts — agent instructions

You are extracting `game_mcts/tournament_server/` (the arena: per-problem
coordinator + sandbox fleet + referee) out of the `game-mcts` repo into its
own repository. **Do this outside the devcontainer**: you need to create and
push to a new GitHub repo, and the devcontainer is not provisioned for that.

This document is self-contained. It was written after verifying the coupling
in the source tree (commit `9bca41c`, 2026-09-06); re-verify the "verified
facts" if the tree has moved since.

## What the arena is

- `server/` — the coordinator: gRPC + HTTP leaderboard, candidate store,
  scheduler, fleet service, tokens/quotas, standings (ELO + metric). Links
  **no** game code; `server:no_problem_code_test` enforces it by nm-scanning
  the linked `problem_server` binary for game symbols.
- `sandbox/` — `common/` (docker mechanics shared by the two below),
  `worker/` (fleet worker: `local` and `docker` backends running
  `WorkOrder`s), `runner/` (standalone gRPC `SandboxService`, a dev tool).
- `referee/` — the one place game code lives: `GameRegistry` maps a name
  (`"risk2"`, `"tictactoe"`, `"bench"`) to a type-erased `GameSession`
  (serialized states/actions as byte strings) plus builtin opponents.
- `client/`, `candidate_api/`, `candidates/` — reference bots and the
  submission API; game-coupled like the referee.
- `proto/` — wire protocols (arena, sandbox_runner, tournament_broker,
  problem, clients). Zero deps on the rest of game-mcts.
- `benchgame/` — a load-test game, self-contained.
- `problems/` — textproto problem definitions (data).
- `tools/arena_admin` — admin CLI.
- Plus two things outside that subtree that belong to the arena:
  - `game_mcts/tools/bench/throughput_benchmark.cc` — depends on
    `referee`/`benchgame`/`server` libs (the only reverse edge in the repo).
  - `mcp_servers/arena_mcp/` — the arena's MCP server (Python).

## Verified facts (why this is a packaging exercise, not an untangling)

- Every `#include` under `server/`, `sandbox/`, `proto/`, `problems/`,
  `tools/` that leaves `tournament_server` resolves to exactly one header:
  `game_mcts/common/process/process.h`.
- `referee/`, `client/`, `candidate_api/`, `candidates/` additionally depend
  on `core/mcts:{game_traits,mcts,minimax,serialization,tournament}` and
  `games/{risk,tictactoe}` — by design; that is the GameRegistry seam.
- Nothing outside the arena subtree depends on it except
  `tools/bench:throughput_benchmark`.
- `common/process` is also used by `core/mcts` in game-mcts, so it **stays**
  in game-mcts; the arena consumes it through the module dep like everything
  else.

## Target layout

New repo (suggested name `game-arena`, module name `game_arena`), **keeping
paths byte-identical** to today:

```
game_arena/
  MODULE.bazel            # new, see below
  .bazelrc                # copied verbatim
  .clang-format           # copied verbatim
  .pre-commit-config.yaml # copied verbatim
  scripts/fix_guards.py   # copied verbatim
  mcp_servers/            # arena_mcp only (BUILD, requirements.txt, README
                          # trimmed to the arena parts)
  game_mcts/tournament_server/...   # the whole subtree, unchanged
  game_mcts/tools/bench/throughput_benchmark.cc + its own BUILD
```

Keeping the `game_mcts/...` prefix means **zero `#include` and zero label
changes inside the moved tree**, and header guards (which encode the path)
stay valid. Stripping the prefix is a possible later cleanup; do not mix it
into this pass.

The arena repo depends on game-mcts as a Bazel module. In the moved BUILD
files, only the files that reference targets *outside* `tournament_server`
change — `//game_mcts/core/...`, `//game_mcts/games/...`,
`//game_mcts/common/process` become `@game_mcts//game_mcts/...`. The complete
list (verified): `referee/BUILD`, `client/BUILD`, `candidate_api/BUILD`,
`candidates/**/BUILD`, `sandbox/common/BUILD`, `sandbox/worker/BUILD`,
`sandbox/runner/BUILD`, and the new `tools/bench/BUILD`. Everything else is
untouched.

`#include "game_mcts/core/mcts/mcts.h"` in an arena file resolves against the
external repo: Bazel puts each direct dep's external-repo root on the quote
include path, the same mechanism that makes `#include "gtest/gtest.h"` work
against `@googletest`. No changes in game-mcts are needed for this. If it
fails, that mechanism is the thing to debug (check the dep is direct, not
transitive-only).

### New MODULE.bazel

game-mcts' own `MODULE.bazel` contains a commented-out sketch of exactly this
consumer module — use it. Required:

```python
module(name = "game_arena", version = "0.1.0")

bazel_dep(name = "game_mcts", version = "0.1.0")
local_path_override(module_name = "game_mcts",
                    path = "/path/to/sibling/game-mcts")  # dev; CI: git_override pinned to a commit

bazel_dep(name = "platforms", version = "1.0.0")
bazel_dep(name = "rules_python", version = "1.6.3")   # arena_mcp py_binary + py_proto_library
bazel_dep(name = "rules_cc", version = "0.2.14")
bazel_dep(name = "rules_shell", version = "0.7.1")    # server:no_problem_code_test is an sh_test
bazel_dep(name = "googletest", version = "1.17.0")
bazel_dep(name = "re2", version = "2025-11-05.bcr.1") # sandbox/worker:build_log
bazel_dep(name = "abseil-cpp", version = "20250814.1")
bazel_dep(name = "protobuf", version = "32.1")
bazel_dep(name = "grpc", version = "1.74.1")
# Token hashing in server/client_registry; version matched to grpc's resolve.
bazel_dep(name = "boringssl", version = "0.20241024.0")

# Still needed: protobuf is not the root module here either. Copy
# protobuf_python_dist_build.patch / protobuf_python_dist_bzl.patch from
# game-mcts.
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

Not needed (game-side only): `pybind11_bazel`, `google_benchmark`,
`boost.*`. Do not copy `MODULE.bazel.lock` — let Bazel regenerate it, then
commit the regenerated lock.

## Steps

1. **Extract with history.** Use `git filter-repo` (install it if missing):
   ```sh
   git clone /path/to/game-mcts game-arena && cd game-arena
   git filter-repo --path game_mcts/tournament_server \
                   --path game_mcts/tools/bench/throughput_benchmark.cc \
                   --path mcp_servers/arena_mcp \
                   --path scripts/fix_guards.py \
                   --path .bazelrc --path .clang-format \
                   --path .pre-commit-config.yaml \
                   --path protobuf_python_dist_build.patch \
                   --path protobuf_python_dist_bzl.patch
   ```
   `throughput_benchmark.cc` lands at its old path; give it its own
   `game_mcts/tools/bench/BUILD` with just that `cc_binary`, its
   `tournament_server` deps unchanged, and framework deps switched to
   `@game_mcts//...` (it has none — it only uses arena + grpc + abseil).
2. Write the new `MODULE.bazel` (above), point `local_path_override` at the
   sibling game-mcts checkout.
3. Fix the listed BUILD files: `//game_mcts/{core,games,common}` →
   `@game_mcts//game_mcts/{...}`.
4. **Verify, in this order** (each gates the next):
   - `bazel build //game_mcts/tournament_server/proto/...` — self-contained.
   - `bazel build //game_mcts/tournament_server/server:problem_server` —
     the pure coordinator; must build with only proto/grpc/abseil/boringssl.
   - `bazel test //game_mcts/tournament_server/sandbox/...` — sandbox tests
     use fake docker/git scripts, no daemon needed. Must pass unmodified.
   - `bazel build //game_mcts/tournament_server/referee/...` — the first
     target that crosses into `@game_mcts`. This is where include-path
     issues would surface.
   - `bazel build //... && bazel test //...` — expect the same 20+ arena
     tests to pass, including `no_problem_code_test`.
   - `bazel test --config=asan //game_mcts/tournament_server/...`
5. Trim `mcp_servers/` to `arena_mcp`: split `mcp_servers/BUILD`,
   `requirements.txt`, `README.md`; keep `arena_mcp/make_stubs.sh` paths
   working (it references `game_mcts/tournament_server/proto/arena.proto`
   relative to the repo root — unchanged under the kept prefix).
6. Create `github.com/geligeli/game-arena` (or the name the user gives) and
   push. Then in the game-mcts repo, open the removal PR: delete
   `game_mcts/tournament_server/`, `mcp_servers/arena_mcp`,
   `tools/bench/throughput_benchmark.cc`, the `boringssl` bazel_dep (only the
   arena's token registry uses it — re-grep to confirm), and the commented
   consumer-module sketch in `MODULE.bazel` (it now lives here for real).
   Do **not** push or merge the removal yourself — that is a review step.
7. Update both AGENTS.md files: game-mcts' loses the tournament-server
   section; this repo gains one (start from that section plus this file's
   "Verified facts").

## Guardrails

- Do not "improve" anything while moving. The split changes no C++.
- The sandbox integration tests assert exact docker argv/scripts from
  fake-docker logs; if one fails, you changed behavior — stop and find it.
- `no_problem_code_test` must keep passing in the new repo; it is the proof
  the coordinator still links no game code.
- The coordinator's contract is the protos in `proto/`; do not bump field
  numbers or "fix" them during the split.

## Follow-ups (not part of the split)

- Per-problem sandbox limits (memory/cpus/user) currently come from worker
  flags; `SandboxSpec` defines them but only `require_container` rides on the
  `WorkOrder`. Deliberately deferred.
- Optional path-prefix strip (`game_mcts/tournament_server/` →
  `tournament_server/`): mechanical include/label/guard sweep, only after
  the split lands and is green.
