# Connect Four — a tournament problem

Write a bot, get rated against everyone else's. Submissions play each other;
there is no grader, because "good" here is only defined against an opponent.

This is the larger of the two [examples](../README.md), and the one that shows
the arena's real extension point: **a game the arena has never heard of, linked
into its referee at build time**.

## Try it

```sh
bazel test //...                                   # the rules, and the config
bazel run //:play                                  # a tournament and your kit
arena_cli spar builtin:greedy                      # in it: yours vs a builtin, here
```

```
Game over: WIN (reason: normal), new ELO 1516.0
...
```

The leaderboard is on <http://localhost:8080>. The reference bot beats
`builtin:random` about 8 times out of 8 and `builtin:greedy` about 6 out of 8,
which is roughly where a submission should start.

## The pieces

| | |
| --- | --- |
| `BUILD` | one `arena_problem()` call: the referee, the broker, the tests, the tournament, the kit |
| `problem.textproto` | the whole problem: what to build, how matches run, what to rank |
| `game/connect4.{h,cc}` | the rules, as a `tournament_broker::GameSession` |
| `game/registry.cc` | **the seam** — defines `GameRegistry()` |
| `bots/bot_api.h` | the board, as a submitter sees it |
| `bots/bot_main.cc` | the harness compiled around every submission |
| `bots/reference/strategy.h` | the starting point for a submission |

## The seam

`@game_arena` declares `GameRegistry()` and defines it nowhere. This repo
defines it, and a referee is that definition plus the arena's game-agnostic
`main()`:

```python
cc_binary(
    name = "match_referee",
    deps = [":registry", "@game_arena//game_arena/referee:referee_main"],
)
```

`arena_problem(registry = "//game:registry")` in the root `BUILD` writes that
rule, and the broker and random client beside it. `:registry` needs
`alwayslink = 1` — nothing depends on it by label, so the linker would
otherwise skip the archive member that satisfies the symbol.

The rules themselves only ever speak bytes:

```
state   "..........................................:0"   42 cells, then whose turn
action  "3"                                              the column to drop into
```

The arena never parses either. It hands the state to a player, takes back an
action, and asks the session whether that was legal — which is why the same
coordinator runs this and a knapsack solver without knowing the difference.

## Writing a bot

One function. Everything else — connecting, the handshake, parsing the board,
serializing the move — is `bots/bot_main.cc`, compiled unchanged around your
header.

```cpp
auto ChooseColumn(const bot::Board &board, std::mt19937 &gen) -> int {
  for (const int col : board.LegalColumns()) {
    if (board.IsWinningMove(col, board.me)) return col;   // take the win
  }
  for (const int col : board.LegalColumns()) {
    if (board.IsWinningMove(col, board.them)) return col;  // deny theirs
  }
  return board.LegalColumns().front();
}
```

Return a column 0-6 with room in it. An illegal move loses the game on the spot:
the referee validates every action, and an invalid one is a loss, not a retry.
`bot::Board` gives you `LegalColumns()`, `After()` and `IsWinningMove()` so a
search is a few more lines.

Every participant is a directory under `bots/`, named after them, with the
BUILD the arena generates: `bots/reference/` is the one everyone starts as. In
a kit `arena_cli` copies it to `bots/<you>/` the first time, and
`//bots/<you>:bot` is the same harness the arena compiles.

## How a submission is built

`submission.harness` in `problem.textproto` names three labels from this repo:

```textproto
harness {
  api_dep: "//bots:bot_api"        # what every submission links
  main_src: "//bots:bot_main.cc"   # provides main(), compiled per submission
  bot_deps: "//bots:bot_deps"      # the rest of the binary's deps
}
```

At submit time the coordinator generates a per-submission `BUILD` from exactly
those, plus the submitted files, plus whatever the submission asked for that is
allowed by `submission.allowed_dep_prefixes`. It is generated rather than
submitted because a submitter who could write their own `BUILD` could write a
genrule, and a genrule runs arbitrary code at build time.

`main_src` is a source, not a dependency, because the entry header arrives as a
`local_define` and those do not propagate from a prebuilt library.

## Running the tournament, and what a participant gets

```sh
bazel run //:play                                # all of the below, and a shell in your kit
bazel run //:tournament                          # the coordinator; a worker is its own process
bazel run //:kit -- --out=/srv/kits/alice --mint=alice --server=$(hostname):50051
```

The kit is a workspace of its own holding only `kit_files` -- here `game/`
(the rules, so a bot can search them) and `bots/` (the API, the harness, the
reference strategy) -- plus the arena's kit surface vendored as `./arena`,
`arena_cli` as a program in `.arena/bin`, `//:mcp_server`, a
`//:match_referee` for `arena_cli spar`, an `ARENA.md` and an `arena.textproto`
generated from `problem.textproto`, and the token in `arena.env` and
`mcp.json`. From it:

```sh
. ./arena.env                             # arena_cli on PATH, address, token
arena_cli submit --wait                   # sends bots/<you>/; again replaces it, once it builds
arena_cli source alice                    # alice's, pulled into bots/alice/
arena_cli spar alice                      # that, then yours against it, here
```

The `kit` section of `problem.textproto` names the starter (`starter_dir`).
A problem whose participants need
more installed than bazel, git and python3 layers its own `oci_image` on
`@game_arena//game_arena/image:kit_base` and names it as
`arena_problem(kit_base = ...)`; this one does not. What they
may read of each other is the separate `source` section; the default, and what
this problem does, is that every submission is readable.

`bazel build //:kit_image` is the same kit as an image -- a build output,
stacked by bazel on a pinned base, with no docker involved and no token
inside: `docker run -it -e ARENA_TOKEN=... TAG` is a shell in the kit, and
`docker run -i TAG bazel run //:mcp_server` is the MCP server for an agent
(`bazel run //:kit_image_load` puts it in your daemon as `connect4-kit`).
`bazel run //:kit_image_issue -- --mint=alice --image=TAG --push` derives
alice's from it: dependencies vendored, cache primed, her token baked in.

`scripts/new_problem.sh match <dir>` copies this out as a repository of its
own, to start a problem from.

## Putting it on a server

The same thing, off this machine: `bazel run //:tournament` from a checkout
on a host people can reach. The coordinator and its workers are processes
there; only submissions run in containers, in the sandbox image that target
loads first. State is `~/.arena/connect4`.

```sh
./deploy.sh                                          # the below, with this repo's names in it

bazel run //:tournament                              # the coordinator
bazel run //:sandbox_image_load                      # what a worker builds and runs in
bazel run @game_arena//game_arena/sandbox/worker:sandbox_worker -- --server=localhost:50051
```

Each participant is one command from that checkout, which mints a token,
reloads the registry and writes their kit -- as a directory (`//:kit --
--out=DIR`), or as their own image with the address and token baked in:

```sh
bazel run //:kit_image_issue -- --mint=alice \
    --server=arena.example.com:50051 --http=arena.example.com:8090 \
    --image=registry.example.com/kit-alice:1 --push
```

```sh
docker run -it registry.example.com/kit-alice:1      # their environment, everything built
```

The referee, the registry and `bots/` ride along inside the sandbox image;
`match { }` needs nothing else at deploy time, because a worker builds
`//:match_referee` from the tree that image carries. [The full
walkthrough](../README.md#deploying-it-on-another-host) covers extra workers,
`arena_admin mint` for a token without a kit, revoking one, and what survives
an upgrade.
