# Tournament Broker

A gRPC server that matches named strategies against each other (or against
built-in strategies) and runs their games turn by turn, enforcing a per-turn
time limit. It keeps persistent ELO ratings, stores every completed game to
disk, and serves a leaderboard over HTTP.

## Running

```
bazel run //game_arena/testgame:broker_server -- \
    --grpc_port=50051 --http_port=8080 --data_dir=tournament_data \
    --turn_timeout_ms=10000 --game_time_budget_ms=0 \
    --rendezvous_timeout_ms=60000 --max_moves_per_game=50000 \
    --hello_timeout_ms=30000 --keepalive_s=60 --shutdown_grace_s=5
```

`--turn_timeout_ms` bounds a single move. It does not bound a game: a strategy
that thinks for just under the limit on every one of thousands of moves stays
within it. `--game_time_budget_ms` bounds the total thinking time per seat per
game (0 = unlimited), which is what makes an automated evaluation run cost a
predictable amount of wall clock.

## Client protocol

One bidirectional `TournamentBroker.Play` stream per game
(`tournament_broker.proto`):

1. Client opens the stream and sends `hello` with `player_name`, `game` (a key
   into the linked registry, e.g. `"nim"`), and `opponent`:
   - `any` (or empty): queue until another client with the same game
     arrives;
   - `builtin:<spec>`: play a built-in immediately. Which specs exist is up to
     the registry — the reference one offers `builtin:random` and
     `builtin:optimal`; a spec may carry knobs, as in
     `builtin:mcts:iterations=N`;
   - `player:<name>`: wait for that one named player, who must name you in
     return. Both sides are paired as soon as the second arrives. Seats
     alternate across a pair's series, so which side connects first does not
     decide who moves first. A partner that never shows up closes this stream
     after `--rendezvous_timeout_ms`.
2. Server sends `game_start` (your seat, opponent name, initial state).
3. On each of your turns the server sends `your_turn` with the serialized
   state and a wall-clock `deadline_unix_ms`. Reply with `action` containing
   the serialized action proto. Missing the deadline loses the game
   (`reason="timeout"`, or `"time_budget"` when it was the game budget rather
   than the per-turn limit that ran out); an invalid action loses with
   `"illegal_action"`; disconnecting loses with `"opponent_disconnect"`.
4. The game ends with `game_over` (result, reason, your new ELO), after which
   the server closes the stream itself. Clients may half-close at any point;
   they no longer have to in order for the server to release the call.

`any` is a FIFO queue, so it cannot express "these two specific players play
each other" — that is what `player:<name>` is for, and it is what lets a
scheduler dispatch both sides of a match to two separate sandbox hosts.

**Draining before `Finish()`.** A client whose deadline expires mid-think will
find its next write rejected, because the server has already finished the call.
It must keep reading until `Read()` returns false anyway: gRPC's synchronous
`Finish()` blocks until the stream is drained, so bailing out on a failed write
hangs instead of reporting the loss. `PlayRemoteGames` handles this; hand-rolled
clients must too.

## Concurrency

The broker uses gRPC's **callback (reactor) API**: each `Play` stream is a
`PlayReactor` driven by completions on gRPC's EventEngine, so a connected or
queued player costs memory rather than a parked OS thread. Idle client count
does not move the server's thread count at all — 200 queued clients cost the
same 20 threads as an empty server, where the previous synchronous handler cost
one thread each (221 threads at 200 clients).

Per-player state lives in a `shared_ptr`-owned `PlayerConnection`, deliberately
split from the reactor: gRPC reclaims the reactor after `OnDone()`, while a
running game may still hold the handle. The reactor is reached through a raw
`Transport*` that `OnDone()` clears under the connection's mutex, so a game
thread mid-`Send()` sees either a live transport or `nullptr`, never a dangling
one.

Games are event-driven too (`game_run.h`, `worker_pool.h`), not one thread
apiece. A `GameRun` advances until it must wait on a remote player, sends
`YourTurn`, arms a deadline on the shared `Timer`, and returns; it resumes when
an action arrives or the deadline fires. Every transition runs on that game's
own `Strand`, so its state is single-threaded by construction and needs no
locking, while costing no thread. CPU-heavy built-in moves (MCTS, minimax) are
bounded by the `WorkerPool` (`--worker_threads`, default
`hardware_concurrency()`) instead of fanning out one thread per game.

Net effect: thread count is flat in load. On a 20-core host, 40 idle clients
plus 30 concurrent search-heavy games cost the same 43 threads as an idle
server.

## Bounding connections

Three limits keep a connection from outliving its usefulness:

- `--hello_timeout_ms` closes a stream that opens and then says nothing.
  Keepalive does not cover this case — the peer is alive, just silent.
- `--keepalive_s` pings otherwise idle connections, so a bot host that is
  powered off mid-game is reclaimed rather than lingering until TCP gives up.
- `--shutdown_grace_s` bounds how long a shutdown waits on stragglers.

On `SIGINT`/`SIGTERM` the broker closes first: it refuses new joins, releases
everyone queued, and aborts games in flight — each of which still writes its
final `GameOver` and persists its record — and only then shuts the gRPC server
down. Doing it the other way round works but is far slower, because the server
sits on the grace period waiting for RPCs that the broker is about to end
anyway. Shutdown with 70 connected clients and 30 running games takes ~200 ms,
independent of `--turn_timeout_ms`.

State and action bytes are opaque to the broker: whatever the game's
`GameSession` produces from `SerializeState()` and accepts in
`ApplySerializedAction()`. Most games use a serialized proto; the reference game
uses a short ASCII string (`"21:0"`, `"3"`) so a failing test stays legible.
Chance nodes are resolved by the server and never require client input.

`random_client.cc` is a complete reference client (plays uniformly random
valid moves):

```
bazel run //game_arena/testgame:random_client -- \
    --name=my-bot --game=nim --opponent=builtin:optimal
```

## Wrapping a typed strategy

The reference client handles bytes directly, which is fine for a random player
and tedious for anything else. A problem that expects real strategies usually
ships a typed wrapper: it deserializes the state into the problem's own type,
calls a strategy, and serializes the action back — the mirror image of the
`GameSession` adapter on the referee side.

That wrapper belongs with the problem, not here, for the same reason the
registry does: it is written against the problem's types. The
[game-mcts](https://github.com/geligeli/game-mcts) repo's
`game_mcts/arena/client/remote_client.h` is a worked example — it joins any
`mcts::tournament::TournamentPolicy<G>` to a broker and owns the whole
`Play`-stream protocol, including drain-before-`Finish`.

## Writing a candidate

A *candidate* is a solution the arena builds and rates automatically — see
[ARENA.md](ARENA.md) for the system around it (the sandbox fleet and the MCP
tools agents drive it with).

**The shape of a candidate is the problem's business, not the arena's.** A
problem that accepts structured submissions declares a harness in its config:

```textproto
submission {
  files_submit_dir: "solutions"
  harness {
    api_dep: "//problem/harness:api"        # what every solution links
    main_src: "//problem/harness:main.cc"   # provides main(), compiled per candidate
    bot_deps: "//problem/harness:client"
    game_define: "PROBLEM_GAME_ALPHA"       # optional compile-time selection
  }
  allowed_dep_prefixes: "//problem/lib:"    # what a solution may additionally use
}
```

At submit time the coordinator generates the candidate's `BUILD` from exactly
those labels and nothing else. It is generated rather than submitted because a
submitter who could write their own BUILD could write a genrule, and a genrule
runs arbitrary code at build time — that is what makes
`allowed_dep_prefixes` enforceable.

The harness main is compiled per candidate rather than depended on: the entry
header and the game arrive as `local_defines`, and those do not propagate from a
prebuilt library.

A problem that leaves `harness` unset accepts only hand-written patches, which
bring their own BUILD. `game_arena/problems/nim.textproto` is that form; the
[game-mcts](https://github.com/geligeli/game-mcts) repo's
`game_mcts/arena/candidate_api` is a worked example of the structured one.

Whatever the harness is, it prints one line the sandbox worker parses:

```
RESULT games=5 wins=3 draws=0 losses=2 elo=1512.4
```

## Leaderboard and history

- `http://localhost:8080/` — HTML leaderboard (auto-refresh).
- `/api/leaderboard` — ratings as JSON.
- `/api/games` — recent games as JSON.

Ratings live in `<data_dir>/ratings.pb` (per game + player name, ELO with
K=32 by default), each completed game is written to
`<data_dir>/games/<game_id>.pb` as a `GameRecord` proto (initial state, every
step with timestamps, result), and `<data_dir>/games/index.jsonl` indexes
them.

## Adding a game

Implement `tournament_broker::GameSession` (`referee/game_session.h`) and hand
back a `GameDescriptor` — a name, a session factory, and a builtin factory.
Then define `GameRegistry()` in a `cc_library(alwayslink = 1)` and link it with
an entry point:

```python
cc_binary(
    name = "match_referee",
    deps = [":my_registry", "//game_arena/referee:referee_main"],
)
```

`game_arena/testgame` is the complete worked example. Nothing needs to change
in this package to add a game — that is the point of the seam.

If your games already exist in some framework, write the adapter once as a
template over that framework's game concept and register instances of it;
`game_mcts/arena/game_session_impl.h` in the
[game-mcts](https://github.com/geligeli/game-mcts) repo does this for
`mcts::SerializableGame`.
