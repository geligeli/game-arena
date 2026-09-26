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

and that defines `:match_referee` (with a
registry), `:config_test`, and three runnable targets:

| | |
| --- | --- |
| `bazel run //:play` | the tournament in the background, a kit minted for you, and a shell in it; leaving the shell stops everything |
| `bazel run //:tournament` | the coordinator, on this checkout. It builds and runs nothing: a worker is a process of its own, which `deploy.sh` and `play` start |
| `bazel run //:kit -- --out=DIR --mint=alice --server=HOST:PORT` | a participant's workspace: `kit_files`, the arena's kit surface as `./arena`, `arena_cli` as a program and MCP server, a README from the config, and a token |
| `bazel build //:kit_image` | the same as an image, as a build output: toolchain and kit, no token, nothing built. `:kit_image_load` / `:kit_image_push` deliver it; `docker run -it -e ARENA_TOKEN=... TAG` uses it |
| `bazel run //:kit_image_issue -- --image=TAG [--push]` | what a build cannot add, added outside one: dependencies vendored and the cache primed. No token or address: `docker run -e ARENA_SERVER=... -e ARENA_TOKEN=...` |
| `bazel build //:sandbox_image` | the sandbox image `sandbox.image` names: this tree on the arena's base. `:sandbox_image_load` / `:sandbox_image_push` deliver it under that name |

`bazel build //:connect4` builds every binary a tournament needs. What a
participant sees is exactly `kit_files`: connect4 ships its rules, harness and
reference bot; knapsack ships the shape of a solution and its README, and
keeps the grader and the cases to itself.

## Running one for real

`bazel run //:play` runs end to end from either directory as it sits: the
problem's tree goes into its sandbox image, and a worker builds every
submission on a fresh copy of that, in a container (there is no mode that
skips it). Nothing clones anything. `scripts/new_problem.sh match|graded
<dir>` is for starting a problem of your own: it copies one out as a repo,
pinned to game-arena by commit.

## Deploying it on another host

Everything above happens on one machine. A tournament other people enter is
the same `bazel run //:tournament`, on a host they can reach, plus two images:
the **sandbox image** every submission is built and run in, and a **kit
image** for participants. The coordinator and its workers are processes on the
arena host, which therefore has a checkout of the problem, bazel and docker.
Only submissions run in containers.

The commands below are connect4's; knapsack's are the same with the other name.

### Before you build

Three things about what you deploy that `play` did not care about:

- **`sandbox.image` has to be a tag you can push and a worker's host can
  pull.** Both examples name `registry.takumi.city/<id>-sandbox:1`, which
  `play` makes locally under that name; change it in `problem.textproto` (and
  in `deploy.sh`) to your registry. Never a tag something else already owns:
  `sandbox_image_push` writes to whatever it names. That name, not a flag, is
  what a worker asks its daemon for. The image holds the problem's tree, so bump
  the tag when the problem or the toolchain changes: submissions are only
  comparable if they were built in the same image, and a moving `:latest`
  quietly breaks that. For a graded problem it holds the hidden cases too, so
  it does not belong on a registry anyone can read.
- **The sandbox fetches its dependencies, unless it is issued.**
  `//:sandbox_image` vendors nothing, so a build in it fetches: knapsack sets
  `sandbox.allow_build_network`, and the first submission in a slot downloads
  the toolchain and builds everything (about three minutes here); after that a
  submission is about twenty seconds. connect4 builds offline instead, in the
  image `bazel run //:sandbox_image_issue` makes -- dependencies vendored, a
  primed cache for a worker's new cache volume -- which `play`, `deploy.sh`
  and `quickstart.sh` issue. Both set a `run_as_user` who is not root.
- **Every package exports its files.** The image's tree is the root package's
  files plus `arena_problem(tree = [...])`: one `filegroup(name = "tree",
  srcs = glob(["**"]))` per package, because a glob does not cross packages.
  A package left out is a build that fails in the sandbox.

### The two images

```sh
cd examples/connect4
docker login registry.example.com
bazel run //:sandbox_image_push                                       # to the name sandbox.image gives it
bazel run //:kit_image_push -- --repository=registry.example.com/connect4-kit --tag=1
```

`./deploy.sh` is the kit push, the tournament below, and a participant's
shell. Neither is a docker build: bazel stacks layers on a base it pulls by
digest and pushes straight to the registry with the logins docker keeps.

`sandbox_image` is `sandbox.image` itself: the arena's sandbox base, the
arena's sources, and this directory's tree at `/workspace`, so a worker needs
no repository at all. The arena host does not need it pushed -- `tournament`
loads it into the local daemon every time, a cached build -- but every other
host that runs a worker does.

### On the arena host

```sh
cd examples/connect4
bazel run //:tournament                 # the coordinator
bazel run //:sandbox_image_load         # what a worker builds and runs in
bazel run @game_arena//game_arena/sandbox/worker:sandbox_worker -- --server=localhost:50051
```

That is the whole install, as two concerns: `problem_server` on 50051 (the
Arena and the SandboxFleet service on one port: submissions in, workers
attached) and 8090 (the leaderboard: read-only, and open, as the arena's
reads are), which builds and runs nothing; and a `sandbox_worker` that starts
every build and every match as a container on this host's daemon. State is `~/.arena/connect4` (`$ARENA_STATE_DIR` to move
it): submissions, ratings and the client registry. Deleting it deletes the
tournament. Each runs in the foreground until Ctrl-C, so keep them
under whatever keeps things running on that host (systemd, tmux).

<http://arena.example.com:8090/> is the leaderboard. The client registry
starts empty: reads are open, every write is gated on a token from that
registry, so until you mint one the tournament is readable and nobody can
submit.

Attaching a *worker* is not gated the same way -- a worker dials in and takes
orders, with no token -- so 50051 belongs somewhere only participants and your
own hosts can reach it, not on the open internet. The port carries submitted
source in one direction and results in the other, and both are things you want
to be sure of.

### Minting tokens, and handing out kits

```sh
bazel run //:kit -- --out=/srv/kits/alice \
    --mint=alice --server=arena.example.com:50051 --http=arena.example.com:8090
```

One command does three things: mints a token and prints it **once**, appends
the client to `~/.arena/connect4/clients.textproto` (as a hash -- a leaked
registry is not a set of usable credentials; the running coordinator reads it
on the token's first use), and writes the kit to `--out`
with that address and that token baked in. `--server` is the arena *as the
participant reaches it*.

Hand it over as a directory:

```sh
tar czf kit-alice.tgz -C /srv/kits alice   # it contains their token: one participant only
```

or, better, as an image -- the whole environment, toolchain included, with
everything already built. One image serves everyone; who they are and where
the arena is go in when it runs:

```sh
bazel run //:kit_image_issue -- --image=registry.example.com/kit:1 --push
```

```sh
T=... # arena_admin mint, below
E="-e ARENA_SERVER=arena.example.com:50051 -e ARENA_NAME=bob -e ARENA_TOKEN=$T"
docker run -it $E registry.example.com/kit:1                        # a shell in /kit, ready to submit
docker run -i  $E registry.example.com/kit:1 arena_cli mcp   # the same, as MCP, for an agent
```

That adds the vendored dependencies and the primed cache to the kit image
bazel built, with regctl.

A token without a kit -- for an image, a CI job, or a participant who already
has the repo:

```sh
bazel run @game_arena//game_arena/tools:arena_admin -- mint --client_id=carol \
    --display_name="Carol" --clients=$HOME/.arena/connect4/clients.textproto
```

The coordinator reads it on the token's first use. Revoking is the same edit in
reverse: delete the client's block from that file and restart the coordinator.

### More capacity

One more `sandbox_worker` is more capacity (give each its own
`ARENA_VOLUME_PREFIX` on a shared daemon). Another host needs docker,
the sandbox image on its daemon, and a checkout to run the worker from:

```sh
docker pull registry.example.com/connect4-sandbox:1
ARENA_VOLUME_PREFIX=c4-worker-1 ARENA_SLOTS=2 \
    bazel run @game_arena//game_arena/sandbox/worker:sandbox_worker -- \
    --server=arena.example.com:50051
```

Workers dial the arena, so there is no inbound port and nothing to register on
the server: adding capacity is starting one more of these. Two things matter.
`ARENA_VOLUME_PREFIX` must be unique per worker on a daemon -- two workers
sharing a bazel output base corrupt it. And the host needs the sandbox image
on its daemon: that is where the tree is, and a worker never pulls.

### Upgrading

The problem config and the rules are in the checkout, and the problem's tree
is inside the sandbox image, so changing either is: pull the checkout, stop
the tournament, `bazel run //:tournament` again. The state directory is what
carries across: submissions, ratings and the client registry survive, so
nobody re-registers and the leaderboard does not reset. Changing the
problem's tree or the toolchain also wants a new `sandbox.image` tag (pushed,
and pulled on every other worker's host) -- it re-bases every comparison,
which is the reason to make that a deliberate, tagged event.
