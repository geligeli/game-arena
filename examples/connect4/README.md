# Connect Four — a tournament problem

Write a bot, get rated against everyone else's. Submissions play each other;
there is no grader, because "good" here is only defined against an opponent.

This is the larger of the two [examples](../README.md), and the one that shows
the arena's real extension point: **a game the arena has never heard of, linked
into its referee at build time**.

## Try it

```sh
bazel test //...                                   # the rules, and the config
bazel run //:broker_server -- --data_dir=/tmp/c4 & # a broker, this game
bazel run //bots:dev_bot -- --name=me --opponent=builtin:greedy --games=6
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

Copy `bots/reference/` to `bots/submissions/<your-id>/` and iterate with
`dev_bot`, which is the same harness the arena compiles.

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
bazel run //:tournament -- --no_container        # coordinator + a local worker
bazel run //:kit -- --out=/srv/kits/alice --mint=alice --server=$(hostname):50051
```

The kit is a workspace of its own holding only `kit_files` -- here `game/`
(the rules, so a bot can search them) and `bots/` (the API, the harness, the
reference strategy) -- plus `//:arena_cli`, `//:mcp_server`, a
`//:broker_server` to iterate against, an `ARENA.md` generated from
`problem.textproto`, and the token in `arena.env` and `mcp.json`. From it:

```sh
. ./arena.env
bazel run //:arena_cli -- submit --name="My bot" --file=bots/reference/strategy.h --wait
```

This directory is inside game-arena's git tree, which a worker cannot clone;
`scripts/new_problem.sh match <dir>` copies it out as a repository of its own.
