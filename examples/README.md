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
| `bazel run //:play` | the tournament in the background, a kit minted for you, and a shell in it; leaving the shell stops everything |
| `bazel run //:tournament` | a coordinator and a local worker, on this checkout; builds the sandbox image if this daemon lacks it |
| `bazel run //:kit -- --out=DIR --mint=alice --server=HOST:PORT` | a participant's workspace: `kit_files`, the arena's kit surface as `./arena`, `arena_cli` as a program, `//:mcp_server`, a README from the config, and a token |
| `bazel build //:kit_image` | the same as an image, as a build output: toolchain and kit, no token, nothing built. `:kit_image_load` / `:kit_image_push` deliver it; `docker run -it -e ARENA_TOKEN=... TAG` uses it |
| `bazel run //:kit_image_issue -- --image=TAG [--mint=bob] [--push]` | what a build cannot add, added outside one: dependencies vendored and the cache primed, and a participant's token. `--prime_cache=false` for the token alone, in seconds |
| `bazel run //:sandbox_image` | the offline sandbox image `sandbox.image` names |
| `bazel build //:tournament_image` | the coordinator and workers as an image, for any host with a docker socket and that sandbox image; `:tournament_image_load` / `:tournament_image_push` deliver it |
| `bazel run //:tournament_image_bundle -- --image=TAG [--push]` | that image plus the repository its workers clone, for a problem whose `repo.url` is a path -- which both of these are |

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
first commit made -- from there `bazel run //:play` runs end to end, given a
`sandbox.image` tag this host can build (every submission is built and run in
a container; there is no mode that skips that).

## Deploying it on another host

Everything above happens on one machine. A tournament other people enter is
three docker images and one `docker run`: the **sandbox image** every
submission is built and run in, the **tournament image** holding the
coordinator and its workers, and one **kit image** per participant. The host
that serves the tournament gets docker and nothing else -- no bazel, no
checkout, no arena source.

The commands below are connect4's; knapsack's are the same with the other name.

### Before you build

Three things about the copy you deploy that `play` did not care about:

- **It has to be a repository of its own.** `scripts/new_problem.sh match
  /srv/src/connect4` copies the example out and makes the first commit. The
  workers clone `repo.url`, and in the tournament image that is the copy of
  this repo baked in at `/opt/arena/problem`.
- **`sandbox.image` has to be a tag you can push and the arena host can
  pull.** Both examples ship a placeholder; change it in `problem.textproto`
  to something like `registry.example.com/connect4-sandbox:1` and **commit
  it**. That name, not a flag, is what a worker asks its daemon for. Bump the
  tag when the toolchain changes: submissions are only comparable if they were
  built in the same image, and a moving `:latest` quietly breaks that.
- **Commit `MODULE.bazel.lock` once it settles.** A worker clones the
  *committed* tree and builds it in a sandbox with no network, so a lockfile
  that no longer matches `MODULE.bazel` makes bazel re-resolve the module
  graph, reach for the BCR, and fail every submission with `Unknown host:
  bcr.bazel.build`. `up` warns when the tree it is serving has uncommitted
  changes; that warning is worth reading.
- **`game_arena` should be pinned by commit in `MODULE.bazel`**, which is what
  `new_problem.sh` writes. With a local `--override_module` instead, a copy of
  that checkout is baked into the images and the tool warns you; the images
  are then only as reproducible as one directory on your laptop.

### On your build host: two images and a push

```sh
cd /srv/src/connect4
docker login registry.example.com
bazel run //:sandbox_image -- --push                                  # the tag sandbox.image names
bazel run //:tournament_image_bundle -- --image=registry.example.com/connect4-arena:1 --push
```

Neither is a docker build, and the build host needs no daemon for either:
bazel stacks layers on a base it pulls by digest, and `--push` goes straight
to the registry with the logins docker keeps.

`sandbox_image` makes `sandbox.image` itself: every external repository
the problem resolves, the C++ toolchain among them, vendored on this host and
added to the arena's sandbox base, so a worker builds submissions in it with
no network at all. (`--tag=other:1` makes it under a different name for a
scratch run -- but what gets deployed has to match the config.)

`tournament_image` is the coordinator as a build output: `problem_server`,
`sandbox_worker`, `arena_tournament`, `arena_admin`, `arena_cli`, the docker
CLI, the arena's kit surface as sources, the built kit image, and this
problem's config and kit files. The arena's binaries are built **from this
workspace**, so they are the arena version the problem depends on.
`tournament_image_bundle` then adds the one thing a build cannot hold -- this
repo with its `.git`, as a clean clone of the committed tree, for the workers
to clone. Every build a tournament does happens in the sandbox image, through
the docker socket, never in this one.

The sandbox image takes a minute or two, nearly all of it compressing the
toolchain; the tournament image is as fast as the build cache is warm, and the
bundle step is seconds.

### On the arena host

```sh
docker login registry.example.com
docker pull registry.example.com/connect4-sandbox:1   # what submissions build and run in
docker pull registry.example.com/connect4-arena:1

docker run -d --name connect4-arena --restart=unless-stopped \
    -p 50051:50051 -p 8090:8090 \
    -v /var/run/docker.sock:/var/run/docker.sock \
    -v connect4-state:/var/arena \
    registry.example.com/connect4-arena:1
```

That is the whole install. What each flag is for:

| | |
| --- | --- |
| `-p 50051` | the Arena and the SandboxFleet service on one port: submissions in, workers attached |
| `-p 8090` | the leaderboard: read-only, and open, as the arena's reads are |
| `-v /var/run/docker.sock` | the workers start every build and every match as a container on **this** daemon. That is also why the sandbox image has to be pulled here first: `up` does try to build one it cannot find, from the repo inside the image, but that wants the network and a while, and building it is the build host's job |
| `-v connect4-state:/var/arena` | submissions, ratings, the client registry and the workers' checkouts. Keep it a volume -- deleting it deletes the tournament |

`docker logs connect4-arena` shows what `bazel run //:tournament` prints
locally, and <http://arena.example.com:8090/> is the leaderboard. The client
registry starts empty: reads are open, every write is gated on a token from
that registry, so until you mint one the tournament is readable and nobody can
submit.

Attaching a *worker* is not gated the same way -- a worker dials in and takes
orders, with no token -- so 50051 belongs somewhere only participants and your
own hosts can reach it, not on the open internet. The port carries submitted
source in one direction and results in the other, and both are things you want
to be sure of.

### Minting tokens, and handing out kits

```sh
docker exec -it connect4-arena arena_tournament kit \
    --mint=alice --server=arena.example.com:50051 --http=arena.example.com:8090
```

One command does four things: mints a token and prints it **once**, appends
the client to `/var/arena/connect4/clients.textproto` (as a hash -- a leaked
registry is not a set of usable credentials), signals the running coordinator
to reload it so the token works immediately, and writes the kit to
`/var/arena/connect4/kits/alice` with that address and that token baked in.
`--server` is the arena *as the participant reaches it*; `localhost` inside a
container is the container.

Hand it over as a directory:

```sh
docker cp connect4-arena:/var/arena/connect4/kits/alice ./kit-alice
tar czf kit-alice.tgz kit-alice        # it contains their token: one participant only
```

or, better, as their own image -- which is the whole environment, toolchain
included, with everything already built:

```sh
docker exec -it connect4-arena arena_tournament kit \
    --mint=bob --server=arena.example.com:50051 --http=arena.example.com:8090 \
    --image=registry.example.com/kit-bob:1 --push
```

```sh
docker run -it registry.example.com/kit-bob:1                        # a shell in /kit, ready to submit
docker run -i  registry.example.com/kit-bob:1 bazel run //:mcp_server  # the same, as MCP, for an agent
docker run -it -e ARENA_SERVER=other:50051 registry.example.com/kit-bob:1
```

That takes about a second: the tournament image carries the kit image bazel
built, and this adds bob's token to it with regctl -- nothing is built, and
the push goes from inside the container, so the registry login has to be in
there (`docker exec -it connect4-arena /opt/arena/bin/regctl registry login
registry.example.com`). What it does not have is a primed cache: bob's first
build in it fetches and compiles. From a checkout of the problem, `bazel run
//:kit_image_issue -- --mint=bob ... --image=... --push` adds the vendored
dependencies and the primed cache as well -- but mints into the registry of
the tournament running *there* (`--clients=`), not into this container's. One
primed image for everyone and a token each (`arena_admin mint`, then `docker
run -e ARENA_TOKEN=...`) avoids the question.

A kit image is one participant's credential: build one per client id, and push
it somewhere only they can pull. A kit `docker cp`'d out of the container is
the same workspace, as cold as the image made here: priming is a full build
of the kit, and the arena host is not where that is done.

Someone who needs a token but not a kit -- a CI job, or a participant who
already has the repo:

```sh
docker exec -it connect4-arena arena_admin mint --client_id=carol \
    --display_name="Carol" --clients=/var/arena/connect4/clients.textproto
docker kill -s HUP connect4-arena      # reload the registry
```

Revoking is the same edit in reverse: delete the client's block from that file
and send the same `SIGHUP`.

### More capacity

The tournament container runs one worker. Another, on any host with a docker
socket and the sandbox image:

```sh
docker run -d --name c4-worker-1 \
    -v /var/run/docker.sock:/var/run/docker.sock \
    -e ARENA_VOLUME_PREFIX=c4-worker-1 -e ARENA_SLOTS=2 \
    registry.example.com/connect4-arena:1 \
    sandbox_worker --server=arena.example.com:50051
```

Workers dial the arena, so there is no inbound port and nothing to register on
the server: adding capacity is starting one more of these. Two things matter.
`ARENA_VOLUME_PREFIX` must be unique per worker -- two workers sharing a bazel
output base corrupt it. And the tournament image is used here because it
carries the problem repo the worker clones: with `repo { url: "." }` the
coordinator hands out `/opt/arena/problem`, a path that exists in that image
and nowhere else. A fleet of hosts that are not this image wants `repo.url` to
be a real git URL.

### Upgrading

The problem config, the rules and the repo are *inside* the tournament image,
so changing any of them is a rebuild:

```sh
bazel run //:tournament_image_bundle -- --image=registry.example.com/connect4-arena:2 --push
# on the arena host
docker pull registry.example.com/connect4-arena:2
docker rm -f connect4-arena
docker run -d --name connect4-arena ... -v connect4-state:/var/arena registry.example.com/connect4-arena:2
```

The volume is what carries across: submissions, ratings and the client
registry survive, so nobody re-registers and the leaderboard does not reset.
Changing the toolchain is the same cycle plus a new `sandbox.image` tag,
committed, built, pushed and pulled -- and it re-bases every comparison, which
is the reason to make that a deliberate, tagged event.
