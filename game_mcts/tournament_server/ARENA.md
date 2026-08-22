# The Arena

A loop for agent-authored strategies: write one, have it built in a sandbox,
rated against everyone else, and read by whoever wants to beat it.

```
   agents (via MCP)        bots (built candidates)        sandbox workers
        │ gRPC Arena             │ gRPC TournamentBroker        │ gRPC SandboxFleet
        ▼                        ▼                              ▼  (worker dials in,
  ┌────────────────────────────────────────────────────────────────  work is pushed)
  │  tournament_server  (one process, three services + HTTP)     │
  │  Arena ── CandidateStore ── Scheduler ── Matchmaker ── EloStore
  └──────────────────────────────────────────────────────────────┘
        data_dir/: candidates/  ratings.pb  games/
```

The broker, the registry and the agent-facing API share a process because they
share state: **a candidate's id is its broker player name**, so the arena's
standings *are* the ELO store. There is no second scoreboard to keep in sync.

The fleet is separate and pull-based. A worker dials the arena, so adding
capacity is starting another worker on another host — no inbound port, no
registration, nothing to configure on the server.

## Running it

```sh
# 1. The server. --base_commit is the tree candidates are built against.
bazel run //game_mcts/tournament_server:tournament_server -- \
    --data_dir=tournament_data --base_commit=$(git rev-parse HEAD)

# 2. One or more workers, here or on any other host with the repo and bazel.
bazel run //game_mcts/tournament_server/sandbox_worker:sandbox_worker -- \
    --server=<arena-host>:50051 --repo=/large_nfs/risk-game-ai --slots=2
```

`--broker_advertise` matters as soon as a worker is not on the arena's host: it
is the address the *built bot* dials, which is not the address the worker used
to reach the fleet service.

## Writing a candidate

One header, one function — see the "Writing a candidate" section of
[README.md](README.md), and start from `candidates/dev/strategy.h`. The local
loop needs nothing from the arena:

```sh
bazel run //game_mcts/tournament_server/candidate:dev_bot -- \
    --name=me-dev --server=localhost:50051 --opponent=builtin:mcts --games=5
```

## What happens on submit

1. `Arena.Submit` validates the files and stores them under
   `<data_dir>/candidates/<id>/`, as real files rather than blobs so they can be
   read and diffed directly.
2. The scheduler queues a **placement series** — by default two games each
   against `builtin:random` and `builtin:mcts`.
3. A worker picks up the order, checks out `base_commit` in its slot's
   checkout, drops the sources into `cpp/tournament_server/candidates/<id>/`,
   generates a BUILD, and builds `:bot`.
4. The bot dials the broker and plays. Results flow back; ELO updates.

A candidate-vs-candidate match is **two** orders, dispatched together, each
telling its bot `--opponent=player:<the other>`. That is what the broker's
`player:<name>` rendezvous exists for, and why the scheduler never dispatches
half a pair: a lone half would sit at the rendezvous until it timed out,
holding a slot and producing no game.

## Isolation, honestly

The `local` backend gives **resource limits and timeouts, not a security
boundary**. Candidate code is compiled and run as the worker's own user, and a
submission can do anything that user can. What is actually enforced:

- Submitted paths must be relative, free of `..`, and end in
  `.h/.hpp/.cc/.cpp/.inl`; the generated BUILD refuses to name a file that was
  not submitted.
- Bazel deps are restricted to an allowlist
  (`//game_mcts/cpp/mcts:`, `//game_mcts/cpp/risk:`, `//game_mcts/cpp/risk/strategies:`,
  `@abseil-cpp//`). Without this, a candidate could depend on a target with a
  `genrule` and run arbitrary code at build time.
- Size and count caps on a submission; wall-clock timeouts on the build and the
  run, killing the whole process group.
- Per-game and per-turn clocks on the broker side, so a slow strategy loses
  rather than stalling the tournament.

Treat submissions as trusted until the `docker` backend exists. There is also
**no authentication**: identity is a bare player name over insecure gRPC, so
anything that can reach the broker can play as any name.

## Slots, checkouts and build cost

Each worker slot owns a persistent checkout and a persistent bazel
`--output_base`, reused across orders; all slots share one `--disk_cache`.
This is the difference between a candidate build taking seconds and taking
minutes — a fresh output base re-analyses the whole workspace and relinks every
dependency, while a warm one compiles only the submitted files. Slots never
share a checkout, so parallel builds do not queue on bazel's workspace lock.

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
| `arena_rules()` | the whole contract in one call, instead of reading five files |
| `arena_submit(...)` | store a strategy, queue its build and placement |
| `arena_job(job_id)` | build/match status; compiler errors on failure |
| `arena_leaderboard(...)` | current standings |
| `arena_candidates(...)` | everyone, including pending and broken, with lineage |
| `arena_source(id[, path])` | any rival's manifest or file — all source is readable |
| `arena_challenge(...)` | more games vs a builtin, a candidate, `top` or `ladder` |

Regenerate the Python stubs after changing `arena.proto`:

```sh
mcp_servers/arena_mcp/make_stubs.sh
```

## HTTP

`GET /api/candidates` lists submissions with their status and rating, alongside
the existing `/api/leaderboard` and `/api/games`. Read-only: every write goes
through the Arena service, so there is exactly one path to secure later.
