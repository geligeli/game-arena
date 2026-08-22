# Tournament Broker

A gRPC server that matches named strategies against each other (or against
built-in strategies) and runs their games turn by turn, enforcing a per-turn
time limit. It keeps persistent ELO ratings, stores every completed game to
disk, and serves a leaderboard over HTTP.

## Running

```
bazel run //game_mcts/tournament_server:tournament_server -- \
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

1. Client opens the stream and sends `hello` with `player_name`, `game`
   (`"risk2"`, `"tictactoe"`), and `opponent`:
   - `any` (or empty): queue until another client with the same game
     arrives;
   - `builtin:<spec>`: play a built-in immediately — `builtin:random`,
     `builtin:minimax` (tictactoe), `builtin:mcts` or
     `builtin:mcts:iterations=N` (risk2);
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
plus 30 concurrent `risk2`-vs-MCTS games cost the same 43 threads as an idle
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

State and action bytes are the serialized per-game protos from
`mcts::GameSerializationTraits` (e.g. `risk_game.proto.RiskState` /
`RiskAction` for `risk2`, `tictactoe.proto.TicTacToeState` / `...Action` for
`tictactoe`). Chance nodes (dice) are resolved by the server and never
require client input.

`random_client.cc` is a complete reference client (plays uniformly random
valid moves):

```
bazel run //game_mcts/tournament_server:random_client -- \
    --name=my-bot --game=tictactoe --opponent=builtin:minimax
```

## Wrapping a typed strategy (C++)

Any `mcts::tournament::TournamentPolicy<G>` — i.e. anything callable as
`policy(game, gen) -> PolicyDecision<G>` — joins a broker through
`remote_client.h`'s `PlayRemoteGames<G>`; the wrapper owns the whole protocol
(deserialize state, call policy, serialize action). Stock adapters:
`ProposerPolicy` (uniform random via a proposer) and `MctsPolicy`
(game-generic MCTS, deterministic and chance games). A full Risk MCTS bot
(`risk_mcts_client.cc`):

```cpp
using game_t = risk_game::RiskState<2>;
using proposer_t = risk_game::RiskProposer<2>;
auto rollout = mcts::MakeShortcutRollout<game_t, proposer_t>(
    &risk_game::ResolveBattleWithExpectationInPlace<2>);
MctsPolicy<game_t, proposer_t, decltype(rollout)> policy{.iterations = 400};
PlayRemoteGames<game_t>(stub.get(), "deep-bot", "risk2", "any", 1, policy,
                        gen);
```

## Writing a candidate

A *candidate* is a strategy the arena builds and rates automatically — see
[ARENA.md](ARENA.md) for the system around it (the registry, the sandbox fleet,
and the MCP tools agents drive it with). It is one header that includes
`candidate/candidate_api.h` and defines exactly one function:

```cpp
auto MakePolicy(const candidate::Params &params) -> candidate::policy_t;
```

`policy_t` is `mcts::tournament::AnyPolicy<game_t>`, so what you return is
open-ended — the stock `MctsPolicy` with an `ActionProposer` of your own, a
different rollout, or a search that is not MCTS at all. None of those types
cross the boundary, so the harness never needs to know which you picked.
`Params` carries `key=value` knobs the arena passes through; every getter falls
back rather than throwing, so an unrecognised knob cannot keep a candidate out
of the tournament.

Everything else — connecting, the handshake, deserializing state, serializing
your action, reporting the result — is `candidate/candidate_main.cc`, compiled
unchanged around your header. Start from
`candidates/dev/strategy.h`, which is the stock Risk MCTS bot plus a
commented proposer skeleton, and iterate against a live broker:

```
bazel run //game_mcts/tournament_server/candidate:dev_bot -- \
    --name=me-dev --server=localhost:50051 --opponent=builtin:mcts \
    --games=5 --params=iterations=800
```

It prints one line the sandbox worker parses:

```
RESULT games=5 wins=3 draws=0 losses=2 elo=1512.4
```

The game is selected at compile time with `CANDIDATE_GAME_RISK2` (default) or
`CANDIDATE_GAME_TICTACTOE`. `candidate_api.h` is therefore header-only: `game_t`
and `kGameName` resolve from that define, so they must be compiled by the target
that sets it rather than inherited from a prebuilt library. Only `Params`, which
does not depend on the game, is compiled once (`candidate_params.h`).

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

Any type satisfying `mcts::SerializableGame` (see
`game_mcts/cpp/mcts/serialization.h` in the game-mcts repo) can be
registered: add one `GameDescriptor` entry in `game_registry.cc` with a
session factory and a builtin factory.
