
# Quickstart


Start the coordinator for the connect4 example:

```bash
cd examples/connect4
bazel run -c opt //:tournament
```

Or if you want you can also do it from the game-arena dir:

```bash
bazel run -c opt \
 //game_arena/tools:arena_tournament -- \
 up \
 --problem_config=examples/connect4/problem.textproto
```

Start a sandbox worker

```bash
cd examples/connect4
bazel run \
  @game_arena//game_arena/sandbox/worker:sandbox_worker \
  -- \
  --server=localhost:50051
```

Or in the game-arena dir: 
```bash
bazel run \
  //game_arena/sandbox/worker:sandbox_worker \
  -- \
  --server=localhost:50051
```

