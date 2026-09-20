# The Arena

A loop for agent-authored strategies: write one, have it built in a sandbox,
rated against everyone else, and read by whoever wants to beat it.

```
   agents (via MCP)                              sandbox workers
        │ gRPC Arena                                  │ gRPC SandboxFleet
        ▼                                             ▼  (worker dials in,
  ┌──────────────────────────────────────────────────────  work is pushed)
  │  problem_server  (one process per problem, + HTTP)  │
  │  Arena ── CandidateStore ── Scheduler ── EloStore    │
  │  ProblemConfig (problems/<id>.textproto)            │
  │  links NO game code -- no_problem_code_test says so │
  └─────────────────────────────────────────────────────┘
        data_dir/: candidates/  ratings.pb  games/

  On a worker, per order, inside containers on a private network:
        match_referee  ◄── bot A          (the game rules live here)
                       ◄── bot B / builtin
```

The coordinator stores submissions, schedules them, and publishes standings.
It runs nothing: **a candidate's id is its player name**, so the arena's
standings *are* the ELO store, and there is no second scoreboard to keep in
sync -- but the matches those ratings come from are refereed on a worker, by
the problem's `match.referee_target`, one process per order.

That split is enforced, not merely intended: `problem_server` links no game
code, and `no_problem_code_test` inspects the linked binary's symbols to keep
it that way. A long-running broker still exists for the local development loop
(`//game_arena/testgame:broker_server`), but nothing rated goes through it.

The fleet is separate and pull-based. A worker dials the arena, so adding
capacity is starting another worker on another host — no inbound port, no
registration, nothing to configure on the server.

## Running it

From a problem repository that calls `arena_problem()` (see
`game_arena/rules/problem.bzl` and the examples), the whole thing is:

```sh
bazel run //:play                            # the tournament, a kit for you, a shell in it
bazel run //:tournament                      # a coordinator and a local worker
bazel run //:kit -- --out=DIR --mint=alice   # a participant's workspace + token
bazel run //:kit_image -- --mint=bob --image=TAG   # the same, as an image; no docker needed
bazel run //:sandbox_image                   # the image sandbox.image names
bazel run //:tournament -- --image=TAG       # the tournament, as a docker image
```

`play` is the dev loop: `tournament` in the background with its log in
`~/.arena/<problem_id>/logs/tournament.log`, a kit minted for `$USER` (the
token is reused on the next run), and a shell in that kit with `ARENA_SERVER`,
`ARENA_TOKEN` and the kit's `arena_cli` on `PATH`; leaving the shell stops
everything.

`kit` writes what a participant gets, and only that: the files `kit_files`
names, the arena's kit surface vendored as `./arena` (`//:kit_surface` --
the game side of the arena, not the coordinator or the fleet), `arena_cli` as
a program in `.arena/bin`, `arena.textproto` telling that CLI what a solution
is made of and where `source` puts what it pulls, `arena.env`, `mcp.json` and
a README generated from the config. It builds the kit once as it writes it,
keeping a bazel disk cache inside it (`--prime_cache=false` to skip), so the
participant's first build is warm and a missing `kit_files` entry is found
here rather than by them.

`kit_image` is the same kit as an image, and the one image here that docker
does not build. It is layered: `//game_arena/image:kit_base` is the arena's
base image (`docker/base/Dockerfile`, pulled by digest) with a kit's user,
directory and `PATH` set on it by rules_oci, and the kit goes on top as a tar.
Nothing runs inside an image made that way, so what a Dockerfile would `RUN`
happens on this host first: `bazel vendor` into `.arena/vendor`, then the
build into `.arena/cache`. Both are run with `--nohome_rc --nosystem_rc` and
the kit carries `--incompatible_strict_action_env`, because a cache only hits
for the build that filled it -- a remote executor's platform properties in a
`~/.bazelrc`, or this shell's `PATH`, are part of every action's key, and the
container has neither. The token, the address and the last few files are added
by the tool with `regctl` after the build, never as a bazel action: an
action's inputs end up in a remote cache. `--push` goes straight to the
registry with the logins docker keeps; without it the image is loaded into
the local daemon when there is one, and left as an archive when there is not.

`tournament` writes the effective config and all state under
`~/.arena/<problem_id>` (`$ARENA_STATE_DIR` to move it), starts
`problem_server` on it with a client registry (created empty: writes always
need a token, and `kit --mint` adds one and has the coordinator reload),
waits for the port, starts `--workers` local `sandbox_worker`s, and forwards
Ctrl-C to all of them. It needs docker and the problem's `sandbox.image`, and
builds that image when the daemon does not have it -- which is the slow part
of a first run. There is no flag that runs a tournament without a sandbox:
`sandbox.image` is required by the config, and a worker links no engine that
could run an order outside a container.

`--image=TAG` builds the same thing as a docker image instead of running it:
the arena's binaries built from the problem's workspace, the problem repo for
the workers to clone, git and the docker CLI, and no toolchain. It runs on
any host with a docker socket and the sandbox image on that daemon:

```sh
docker run -d --name c4-arena --restart=unless-stopped -p 50051:50051 -p 8090:8090 \
    -v /var/run/docker.sock:/var/run/docker.sock -v c4-arena:/var/arena TAG
docker exec -it c4-arena arena_tournament kit --mint=bob --server=HOST:50051 \
    --image=REG/kit-bob --push                   # a participant, from inside
docker run -d -v /var/run/docker.sock:/var/run/docker.sock \
    -e ARENA_VOLUME_PREFIX=w2 TAG sandbox_worker --server=HOST:50051   # more capacity
```

Inside, `arena_tournament up` is what `docker run` starts, and `kit` works
because the image carries `kit_files` and the registry label in its
environment. State is the `/var/arena` volume. Or by hand, which is what all
of those run:

```sh
# 1. The coordinator. One server per problem; --problem_config says which.
bazel run //game_arena/server:problem_server -- \
    --problem_config=game_arena/problems/nim.textproto \
    --data_dir=tournament_data --base_commit=$(git rev-parse HEAD)

# 2. One or more workers, here or on any other host with bazel and docker.
bazel run //game_arena/sandbox/worker:sandbox_worker -- \
    --server=<arena-host>:50051
```

**A worker takes one flag.** Which repository to build, which image to build it
in, what the sandbox may do, how long a turn may take -- all of it arrives on
each order, from the problem's config. The reason is the one
`WorkOrder.base_commit` already gives: two submissions are only comparable if
they were built the same way, and a fleet whose hosts were each configured by
hand is a fleet that cannot promise that. A worker with its own `--docker_image`
could quietly rate one problem's submissions against two different toolchains.

Which engine an order runs on is the problem's choice too: it names an image or
it does not. A worker holds both and refuses an order it has no engine for,
rather than running submitted code unsandboxed.

Three things are facts about the host rather than the problem, and come from
the environment so that the flag surface stays at one:

| | |
|---|---|
| `ARENA_MACHINE_CLASS` | what kind of host this is, e.g. `bench-c7i`. Nothing can derive a semantic label, and a graded problem can require one -- unset, this worker refuses those orders |
| `ARENA_SLOTS` | orders at once; how much of this box to lend the arena. Default 2 |
| `ARENA_WORK_DIR` | where per-slot checkouts live, and the process engine's output bases and cache. Default `/tmp/arena_sandbox`. Keep it off the repo: a work dir inside makes `bazel test //...` descend into the worker's own clone |
| `ARENA_VOLUME_PREFIX` | names the docker volumes a container's bazel output bases and disk cache persist in (`<prefix>-slot<N>-output_base`, `<prefix>-disk_cache`). Default `arena-<hostname>`; two workers on one daemon must differ |
| `ARENA_BIND_OUTPUT_BASE`, `ARENA_BIND_DISK_CACHE` | optional. Host directories, as the docker daemon resolves them, to bind-mount for those caches instead of volumes -- a local disk you can inspect or share with your own builds. Nothing needs them; they are a performance choice |

There is nothing for a bot to dial across the network either: a match's referee
is started by the worker that runs the match, on a private network beside the
two bots. A worker needs a route to the fleet service and a docker socket, and
nothing else: the daemon can be the host's, reached from inside a container,
or a remote `DOCKER_HOST`, because nothing on the worker's filesystem is ever
bind-mounted into a sandbox.

## Writing a candidate

What a solution looks like is set by the problem's `submission.harness` — see
the "Writing a candidate" section of [README.md](README.md). Whatever it is, the
local loop needs nothing from the arena: build the problem's bot target and
point it at a broker you run yourself.

```sh
# Against a local broker, not the arena.
bazel run //game_arena/testgame:broker_server -- --port=50051
bazel run //game_arena/testgame:random_client -- \
    --name=me-dev --server=localhost:50051 --game=nim --opponent=builtin:optimal
```

## What happens on submit

1. `Arena.Submit` validates the submission and stores it under
   `<data_dir>/candidates/<id>/` as `patch.diff`, with the files it adds
   extracted beside it so they can be read and grepped directly.

   **A submission is a patch.** The structured form — a list of files plus an
   `entry_header` — is a convenience: the server turns it into an add-only diff
   under the problem's `files_submit_dir`, *generating the BUILD file*, so
   everything downstream handles one form and the worker only ever runs
   `git apply`. Generating the BUILD is also what keeps the dependency
   allowlist enforceable; a submitter who could write their own could write a
   `genrule`, and a `genrule` runs arbitrary code at build time.
2. The scheduler queues a **placement series** — by default two games each
   against `builtin:random` and `builtin:mcts`.
3. A worker picks up the order, checks out `base_commit` in its slot's
   checkout, `git apply`s the patch (both sides, for a candidate-vs-candidate
   match), and builds the problem's targets.
4. The worker starts a `match_referee` and the bot(s) beside it, on a private
   network. The referee plays the games and prints one `RESULT` line.
5. The tally flows back over the fleet stream; the coordinator updates ELO.

A candidate-vs-candidate match is **two** orders, dispatched together, each
telling its bot `--opponent=player:<the other>`. That is what the broker's
`player:<name>` rendezvous exists for, and why the scheduler never dispatches
half a pair: a lone half would sit at the rendezvous until it timed out,
holding a slot and producing no game.

## Isolation, honestly

Both backends are the same engine now, differently configured:
`game_arena/sandbox/exec` runs a job of phases and steps and knows nothing
about orders, and `sandbox/worker/order_job.h` is what turns an order into
one. The dev runner (`sandbox/runner`) runs on it too, which is how it stopped
being the un-hardened counterexample -- it passed `--cap-add SYS_ADMIN` with
no network restriction while the fleet dropped every capability. Isolation is
one function that every container goes through.

The process backend gives **resource limits and timeouts, not a security
boundary**: whatever it runs is compiled and run as the user running it. No
worker has one. `sandbox_worker` links the container engine alone, so an
order it cannot isolate is an order it hands back -- and `exec:process_engine`
is visible only to the tests that need an engine which is not a boundary, to
have something for `capabilities().isolates` to be false about.

The `docker` backend is where the real claim lives, and moving the referee into
the sandbox is what made it possible:

- **No network by default.** The build and a graded run get `--network=none`; a
  match gets a per-order `--internal` bridge, so the two bots reach their
  referee and nothing else. There is no longer an outside broker to dial, which
  is what forced `--network=host` before.
- **Nothing mounted from the worker.** The tree is exported with `tar` and
  copied, with the patches, into per-job docker volumes through the daemon
  (`docker create` + `docker cp`); the bazel output base and disk cache
  persist in named volumes. So there is no overlay to assemble, no
  `mount(8)` on the host or in the sandbox, and no path that has to exist on
  both sides of a docker socket. A host that wants its caches on a local disk
  it can inspect opts into bind mounts for exactly those two
  (`ARENA_BIND_OUTPUT_BASE`, `ARENA_BIND_DISK_CACHE`); nothing requires it.
- **No capabilities.** With nothing to mount, every container runs
  `--cap-drop=ALL` with `--security-opt=no-new-privileges`, and there is no
  privileged mode left to fall back to.
- **A read-only root filesystem**, a non-root `--user` (the loader chowns the
  copied tree to it), and cgroup caps on memory, CPU and pids.

Kits are the other side of that line: a participant's development environment
is their own machine, with whatever access they give it. Only what they
submit runs under the sandbox.

What is enforced above that, at submit time:

- Every path a patch touches must be relative, free of `..`, and pass the
  problem's `allow_paths`/`deny_paths` globs (deny wins). Patch size, file count
  and hunk count are bounded.
- For the structured form, the BUILD file is **generated, never submitted**, and
  bazel deps come from an allowlist. A submitter who could write their own BUILD
  could write a `genrule`.
- **A problem that lets patches touch BUILD files has given that last one up**,
  deliberately, and must rely on the sandbox instead.

The image is still trusted — it carries bazel — and the network being
closed means the image must carry the repo's external dependencies, since a
module fetch will fail. That failure is correct: it is a submission depending
on something the problem did not offer. The C++ toolchain is one of those
dependencies: the arena depends on hermetic-llvm, a bazel module carrying
clang, libc++ and compiler-rt, so the compiler is pinned by
`MODULE.bazel.lock` rather than by whatever the image's distro ships, and a
kit, a developer's checkout and the sandbox all build with the same one.
`bazel run //:sandbox_image` builds such an image from the `sandbox` target
of `game_arena/image/Dockerfile`: a small base with bazel and no compiler,
`bazel vendor` of the problem's `MODULE.bazel` into `/opt/arena/vendor`, and
a system bazelrc pointing bazel at it. A `game_arena` overridden with a local path is
copied into the image too, with a warning, because `bazel vendor` only
symlinks local overrides.

## Slots, checkouts and build cost

Each worker slot owns a persistent checkout and a persistent bazel
`--output_base`, reused across orders; all slots share one `--disk_cache`.
The docker backend keeps the same shape: the checkout stays on the worker (git
runs there) and each order's copy of it is loaded into a volume, while the
`--output_base` and the disk cache live in named volumes that every order of
that slot mounts. This is the difference between a candidate build taking
seconds and taking minutes — a fresh output base re-analyses the whole
workspace and relinks every dependency, while a warm one compiles only the
submitted files. Slots never share an output base, so parallel builds do not
queue on bazel's lock.

## Two ways to be scored

A problem either plays its submissions against each other or runs and measures
them, and `ProblemConfig` says which. Everything above the standings — the
scheduler, the Arena service, the HTML table, the MCP tools — sees rows and a
score label, not ratings or milliseconds.

| | match problem | graded problem |
|---|---|---|
| config | `match { … }` | `grade { … }` |
| an order | build both sides, referee N games | run a command N times |
| result | W/D/L tally | a metric per run |
| standings | ELO, keyed `(problem_id, submission_id)` | the primary metric, in its direction |
| `Evaluate` | `match { opponent, games }` | `grade { repeats }` |

**The coordinator owns the standings.** A match's referee keeps its own ratings
while it plays, but they die with its container: they exist so a game has
somewhere to record itself, not to be authoritative. What crosses the wire is a
tally, and `EloStandings::Record` turns it into a rating once, on the machine
that holds the store. It applies the tally *game by game* — ELO is path
dependent, so a 6–4 result folded in one lump is a different number.

### The metric contract

A graded command writes JSON to the path in `$ARENA_REPORT`:

```json
{"metrics": {"wall_ms": 1234.5, "peak_rss_mb": 91.2}}
```

JSON so a command can emit it with `printf` and no library. A `RESULT wall_ms=…`
line on stdout is accepted as a fallback, matching what the match harness already
prints. Keys the problem does not rank on are dropped rather than rejected — a
benchmark reporting extra numbers is normal. A **nonzero exit is never scored**,
whatever it printed: a number from a failed run looks like a result.

`GradeSpec.repeats` and `aggregate` exist because one timing is noise. And
`require_machine_class`, with the worker's `--machine_class`, is what stops a
board from ranking the fleet: a wall-clock number from a laptop and one from a
server are not the same measurement, which is why the host that produced each
row is stored beside it.

## The MCP surface

`mcp_servers/arena_mcp/server.py` is a thin stdio→gRPC shim. Two things about
it are load-bearing for an agent loop that has to stay cheap:

- `arena_submit` takes **paths, not contents**. The agent just wrote the file;
  making it paste the code back would double the cost of every iteration.
- Build failures come back as extracted compiler errors. The worker compacts
  the bazel log before it crosses the wire, so the full log is never stored,
  forwarded or re-served.

| Tool | What it is for |
|---|---|
| `arena_rules()` | *this server's* problem, from its config — not a README that may describe another |
| `arena_submit(...)` | store a solution — `paths=[…]` or `patch_path=…` — and queue it |
| `arena_job(job_id)` | build/match status; compiler errors on failure |
| `arena_leaderboard(...)` | current standings |
| `arena_candidates(...)` | everyone, including pending and broken, with lineage |
| `arena_source(id[, path])` | a candidate's manifest or file, as far as the problem's `SourcePolicy` allows |
| `arena_evaluate(...)` | more games vs a builtin, a candidate, `top` or `ladder`; or more measurement runs |

Regenerate the Python stubs after changing `proto/arena.proto`:

```sh
mcp_servers/arena_mcp/make_stubs.sh
```

## Who may submit

Writes are gated on an `x-arena-token` metadata header; reads are not. The
leaderboard is meant to be public and readable source is the point of the
arena, so `GetSource`, `ListCandidates`, `GetJob`, `Leaderboard` and
`GetProblem` stay open. Only `Submit` and `Evaluate` spend the fleet.

A problem can narrow the reading (`source { visibility: ... }` in its config):
`ALL` is the default above, `OWN` serves each participant only their own
submissions -- which makes reads token-gated too -- and `NONE` serves nobody's.
It is enforced in `ArenaService` on `GetSource` *and* on the `patch` bytes of
every manifest `GetCandidate`, `ListCandidates` and `Leaderboard` return,
because a stored patch is source. A kit's `arena.textproto` and `ARENA.md`
describe the rule; only the coordinator applies it.

Tokens are **admin-provisioned** — there is no registration RPC, which is what
makes a quota mean anything. Mint one:

```sh
bazel run //game_arena/tools:arena_admin -- \
    mint --client_id=some-agent --display_name="Some Agent"
```

It prints the token once and the registry block to paste into the file named by
`--clients`. What is stored is the token's SHA-256, so a leaked registry file is
not a set of usable credentials. `SIGHUP` reloads it; a reload that fails to
parse keeps the running set rather than locking everyone out.

Without `--clients` the server logs a warning and leaves writes open, which is
fine for a single-agent loop and nothing else; `arena_tournament up` never
runs it that way, and `kit --mint` does the mint and the reload in one step.
It is a bearer token over
whatever transport the operator configured — if that is insecure gRPC, the
token is visible on the path, and terminating TLS in front is the operator's
job.

**`author` comes from the token**, not the request. A quota you can enforce
beside a credit you cannot is only half a system.

### Quotas

Two limits, defaulted in `ProblemConfig.clients` and overridable per client:
`max_active_evaluations` (1 by default — a client waits for its own result
before spending the fleet on another guess) and `max_queued_jobs`, which covers
`Evaluate` rematches as well as fresh submissions.

Over quota, `Submit` returns `RESOURCE_EXHAUSTED` naming the running job, and
**stores nothing**. That is not incidental: the check and its claim are one
locked step inside the scheduler, granted *before* the submission is written, so
two concurrent submits from one client cannot both pass and a refused one leaves
nothing on disk. `Scheduler::Reservation` is that slot; its destructor hands it
back if the store then rejects the submission.

`cancel_running: true` aborts the client's in-flight work and takes its place,
for an agent that already knows its running candidate is superseded. The old job
ends `CANCELLED`, not `FAILED` — an agent polling needs to tell "you replaced
it" from "it broke". Cancellation reaches work already under way: the local
backend kills the step's process group (bazel spawns a tree, so the group, not
the parent), and the docker backend stops the order's containers, whose names it
derives rather than remembers.

## HTTP

`GET /api/candidates` lists submissions with their status and rating, alongside
the existing `/api/leaderboard` and `/api/games`. Read-only: every write goes
through the Arena service, so there is exactly one path to secure later.
