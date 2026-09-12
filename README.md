# game-arena

A framework for running competitive programming problems: agents submit
solutions, the arena builds them in a sandbox, runs them, and ranks them.

A *problem* is a config file. It says which repository a submission patches,
what to build, how to run it, and how to score it — a two-player match refereed
by a binary you supply, or a graded command that writes a metric. "Play Risk"
and "make this benchmark faster" are both problems here.

```
game_arena/
  proto/      wire protocols: arena, tournament_broker, problem
  server/     the coordinator: submissions, scheduling, ELO/metric standings, HTTP
  sandbox/    exec/ (the engine), common/ (docker mechanics),
              worker/ (fleet), runner/ (dev tool)
  referee/    the match loop and the broker protocol
  client/     the generic reference client
  testgame/   Nim: the arena's own game, and the reference registry
  problems/   nim.textproto, the reference problem
  rules/      arena_problem: the one macro a problem repo calls
  tools/      arena_admin, arena_cli, arena_tournament
  common/     small subprocess wrapper
```

Start with [game_arena/ARENA.md](game_arena/ARENA.md) for the submission loop
and [game_arena/README.md](game_arena/README.md) for the broker protocol.

## The arena knows nothing about your problem

This is the property the whole design protects, so it is worth stating plainly.

**Rules arrive at link time.** `referee/game_registry.h` *declares*

```cpp
auto GameRegistry() -> const std::map<std::string, GameDescriptor> &;
```

and defines it nowhere. `referee/game_session.h` is the contract a problem
implements: serialize a state, say whose turn it is, validate and apply a
serialized action, report an outcome. Byte strings throughout — the arena never
learns what a move is.

A referee binary is *a registry plus a game-agnostic entry point*:

```python
cc_binary(
    name = "match_referee",
    deps = [":my_registry", "@game_arena//game_arena/referee:referee_main"],
)
```

`referee:referee_main`, `referee:broker_server_main` and
`client:random_client_main` are `alwayslink` libraries holding `main()`. Your
registry needs `alwayslink = 1` too: nothing depends on it by label, it exists
to satisfy an undefined symbol.

`game_arena/testgame` is a complete worked example in about 130 lines.

**Everything else arrives as config.** `match.referee_target` is the bazel
label of the referee to build. `submission.harness` carries the labels a
generated candidate BUILD is written against.
`submission.allowed_dep_prefixes` bounds what a solution may depend on. None of
these are compiled into the coordinator.

**And it is enforced.** `server:no_problem_code_test` nm-scans the linked
`problem_server` for `GameRegistry`, `GameSession` and the test game's
namespace. The coordinator stores submissions, schedules them and publishes
standings; the moment it can referee a match, someone will make it.

## Examples

Two complete problems live in [`examples/`](examples), one of each shape, each
its own bazel workspace:

- [`examples/knapsack`](examples/knapsack) — a **graded** problem. A grader you
  write scores each submission; no game, no registry, no referee.
- [`examples/connect4`](examples/connect4) — a **tournament** problem. Your own
  game linked into the arena's referee, submissions rated by ELO.

Neither is referenced from anything under `game_arena/`, which is the point.
Start there if you are adding a problem.

## From a problem to a tournament

A problem repository is `MODULE.bazel` + `problem.textproto` + the rules or
the grader + one line of BUILD:

```python
load("@game_arena//game_arena/rules:problem.bzl", "arena_problem")

arena_problem(
    name = "connect4",
    config = "problem.textproto",
    registry = "//game:registry",              # match problems only
    kit_files = ["//game:kit", "//bots:kit"],  # what a participant receives
)
```

That defines everything the two examples run:

```sh
bazel test //...                                     # the rules, and the config
bazel run //:tournament -- --no_container            # a coordinator + a local worker
bazel run //:kit -- --out=/srv/kits/alice --mint=alice --server=$(hostname):50051
bazel run //:sandbox_image                           # the offline sandbox image
bazel build //:connect4                              # every binary a tournament needs
```

`kit` writes a participant's workspace: only the files `kit_files` names,
plus the arena's CLI and MCP server as `bazel run //:arena_cli` and
`//:mcp_server`, a README generated from the config, and a freshly minted
token in `arena.env` and `mcp.json`. The grader, the cases and the tournament
config stay behind. `scripts/new_problem.sh match|graded <dir>` scaffolds a
new repo from an example.

## Build

Bazel 8.x (Bzlmod). No configuration needed — the repo depends on nothing
outside the BCR.

```sh
bazel build //...
bazel test //...
bazel test --config=asan //...   # also tsan / ubsan / msan; see .bazelrc
```

## Try it

```sh
# A broker with the reference game, plus a leaderboard on :8080.
bazel run //game_arena/testgame:broker_server -- --port=50051 --http_port=8080

# Two random players.
bazel run //game_arena/testgame:random_client -- --name=alice --game=nim
bazel run //game_arena/testgame:random_client -- --name=bob   --game=nim
```

## Relationship to game-mcts

The arena grew inside [game-mcts](https://github.com/geligeli/game-mcts) and was
extracted from it with history. game-mcts is now a consumer: it supplies its own
registry for Risk and TicTacToe under `game_mcts/arena/`. Nothing here depends
on it.
