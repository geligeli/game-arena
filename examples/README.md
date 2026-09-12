# Examples

Two complete problems, one of each shape the arena supports. Each is its **own
bazel workspace** with its own `MODULE.bazel` — that is what a problem repo is,
and building one here proves the arena works from outside its own tree. They
live in this directory so the two can be read side by side; nothing in
game-arena's build refers to them.

| | [`knapsack/`](knapsack) | [`connect4/`](connect4) |
| --- | --- | --- |
| Shape | graded | two-player match |
| Scored by | a grader you write | ELO from played games |
| Needs a registry | no | yes |
| Config block | `grade { ... }` | `match { ... }` |
| Ranking | `METRIC` | `ELO` |

Both run standalone:

```sh
cd knapsack && bazel test //...
cd connect4 && bazel test //...
```

## Which shape is mine?

**Graded** if a submission can be scored by running it: a benchmark, a solver, a
model, anything where one number falls out of one command. You write a program
that runs the submission and writes `{"metrics": {...}}` to `$ARENA_REPORT`; the
arena builds, runs and ranks. There is no game, no registry and no referee.

**Match** if submissions have to play each other, because "good" is only defined
relative to an opponent. You implement `GameSession` — serialize a state, apply
an action, report an outcome — and link a `GameRegistry()` into a referee
binary. The arena runs the matches and keeps the ratings.

## What the arena never learns

Worth noticing in both examples: no label, path or name from these directories
appears anywhere in `game_arena/`. A problem reaches the arena through exactly
two channels.

**Configuration**, for everything the coordinator handles — which targets to
build (`build.targets`), what to run (`grade.argv` / `match.referee_target`),
what a submission may depend on (`submission.allowed_dep_prefixes`), and what a
generated submission BUILD is written against (`submission.harness`).

**A linked symbol**, for the rules — `GameRegistry()`, which the arena declares
and never defines. A referee is your registry plus the arena's match loop:

```python
cc_binary(
    name = "match_referee",
    deps = [":registry", "@game_arena//game_arena/referee:referee_main"],
)
```

That is the whole extension surface. If you find yourself needing to change
something under `game_arena/` to add a problem, something is wrong with the
seam, not with your problem.

## From a problem to a tournament

Each example's root `BUILD` is one call:

```python
load("@game_arena//game_arena/rules:problem.bzl", "arena_problem")

arena_problem(
    name = "connect4",
    config = "problem.textproto",
    registry = "//game:registry",              # match problems only
    kit_files = ["//game:kit", "//bots:kit"],  # what a participant receives
)
```

and that defines `:match_referee`, `:broker_server`, `:random_client` (with a
registry), `:config_test`, and three runnable targets:

| | |
| --- | --- |
| `bazel run //:tournament -- [--no_container]` | a coordinator and a local worker, on this checkout |
| `bazel run //:kit -- --out=DIR --mint=alice --server=HOST:PORT` | a participant's workspace: `kit_files`, `//:arena_cli`, `//:mcp_server`, a README from the config, and a token |
| `bazel run //:sandbox_image` | the offline sandbox image `sandbox.image` names |

`bazel build //:connect4` builds every binary a tournament needs. What a
participant sees is exactly `kit_files`: connect4 ships its rules, harness and
reference bot; knapsack ships the shape of a solution and its README, and
keeps the grader and the cases to itself.

## Running one for real

The sandbox worker clones `repo.url` and runs bazel **at its root**, so a
problem's workspace has to be a repository of its own. These two live inside
game-arena's tree so they can be read side by side, which means a worker
cannot clone them as they are. `scripts/new_problem.sh match|graded <dir>`
copies one out as a standalone repo, pinned to game-arena by commit, with a
first commit made -- from there `bazel run //:tournament -- --no_container`
runs end to end.
