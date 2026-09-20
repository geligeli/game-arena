# game-arena

A framework for running competitive programming problems: agents submit
solutions, the arena builds them in a sandbox, runs them, and ranks them.

A *problem* is a config file. It says which repository a submission patches,
what to build, how to run it, and how to score it — a two-player match refereed
by a binary you supply, or a graded command that writes a metric. "Play Risk"
and "make this benchmark faster" are both problems here.

```
game_arena/
  proto/      wire protocols: arena, tournament_broker, problem, kit
  server/     the coordinator: submissions, scheduling, HTTP
  standings/  how results are rated, kept and shown; shared with a kit's broker
  sandbox/    exec/ (the engine), common/ (docker mechanics),
              worker/ (fleet), runner/ (dev tool)
  referee/    the match loop and the broker protocol
  client/     the generic reference client
  cli/        arena_cli: the participant's client, installed into every kit
  testgame/   Nim: the arena's own game, and the reference registry
  problems/   nim.textproto, the reference problem
  rules/      arena_problem: the one macro a problem repo calls
  tools/      arena_admin, arena_tournament (operator tooling)
  common/     small subprocess wrapper
```

The first five of those, plus `cli/` and the MCP server, are the **kit
surface** (`//:kit_surface`): the packages a participant's workspace builds
against, vendored into every kit. The coordinator, the fleet and the sandbox
are not among them.

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
[`examples/README.md`](examples/README.md#deploying-it-on-another-host) walks
one of them from a checkout to a tournament on another host: the sandbox and
tournament images, `docker run` on a box with nothing but docker, a kit image
per participant, and minting their tokens from inside the container.

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
bazel run //:play                                    # all of it, and a shell in your kit
bazel run //:tournament                              # a coordinator + a local worker
bazel run //:kit -- --out=/srv/kits/alice --mint=alice --server=$(hostname):50051
bazel build //:kit_image                             # the kit as an image: a build output
bazel run //:kit_image_issue -- --mint=bob --image=REG/kit-bob --push   # + a primed cache, + bob's token
bazel run //:sandbox_image                           # the offline sandbox image
bazel build //:tournament_image                      # the tournament, as an image
bazel run //:tournament_image_bundle -- --image=REG/c4-arena --push   # + the repo its workers clone
bazel build //:connect4                              # every binary a tournament needs
```

`kit` writes a participant's workspace: only the files `kit_files` names, the
arena's kit surface vendored beside them as `./arena`, `arena_cli` as a
program in `.arena/bin` (sourcing `arena.env` puts it on `PATH` with the
address and the token), `arena.textproto` saying what that CLI does by
default, an MCP server as `bazel run //:mcp_server`, and a README generated
from the config. The grader, the cases, the tournament config and the rest of
the arena stay behind. By default the kit is built once as it is written, so
the participant's first build is warm. `bazel build //:kit_image` is the same
workspace as an image: the kit's tree at `/kit`, stacked by rules_oci on a
base pulled by digest. It is a build output -- cached, reproducible, made
without docker -- and it is anyone's: `docker run -it -e ARENA_TOKEN=... TAG`
is a shell in the kit, `docker run -i TAG bazel run //:mcp_server` the MCP
server on stdio. `:kit_image_load` and `:kit_image_push` put it in a daemon or
a registry. The two things a build cannot put in an image are added outside
one, by `:kit_image_issue`, as layers on the built image: the kit's
dependencies vendored and its cache primed (that is bazel, run on the kit),
and a participant's token (a secret, which a remote cache would keep). The
base is the one Dockerfile that is built by hand, `docker/base/Dockerfile`; a
problem that needs more in its kits layers its own `oci_image` on
`//game_arena/image:kit_base` and names it as `arena_problem(kit_base =
...)`.

The other two images follow the same split. `bazel build //:tournament_image`
is the coordinator and its workers -- the arena's binaries, the docker CLI, the
kit image, the problem's config and kit files -- to `docker run` on any host
with a docker socket and the sandbox image; `tournament_image_bundle` adds the
repository the workers clone, which a build cannot hold, for a problem whose
`repo.url` is a path. Inside, `docker exec ... arena_tournament kit --mint=bob
--image=...` admits a participant in about a second, because the image carries
the built kit image and only the token is added. `bazel run //:sandbox_image`
is the arena's sandbox base with every dependency the problem resolves added
to it -- the result of `bazel vendor`, so that one is a `run`. None of the
three is a docker build, and making any of them needs no daemon. See
`game_arena/ARENA.md`. `scripts/new_problem.sh match|graded <dir>` scaffolds a
new repo from an example.

Every submission is built and run in a container. `sandbox.image` is required,
a worker links no other engine, and `up` builds the image if the daemon does
not have it -- there is no flag anywhere that runs submitted code on the host,
because a submission that may write a BUILD file can run anything at build
time.

## Build

Bazel 8.x (Bzlmod). No configuration needed — the repo depends on nothing
outside the BCR.

```sh
bazel build //...
bazel test //...
bazel test --config=asan //...   # also tsan / ubsan; see .bazelrc
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
