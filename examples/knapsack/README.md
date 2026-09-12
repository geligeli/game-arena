# Knapsack — a graded problem

Pack items into a knapsack. Submissions are scored by a grader you write, not by
playing anyone.

This is the smaller of the two [examples](../README.md), and the smaller of the
two shapes: **no game, no registry, no referee**. The arena builds a submission,
runs one command, and reads the numbers that command leaves behind.

## Try it

```sh
bazel build //...
ARENA_REPORT=/tmp/report.json bazel-bin/grader/grade \
    --solution=$PWD/bazel-bin/solutions/reference/solve --cases=cases
cat /tmp/report.json
```

```
medium: value 44 of 44 (100%)
small: value 13 of 13 (100%)
tight: value 11 of 11 (100%)
{"metrics": {"cases": 3, "infeasible": 0, "score": 100, "solved": 3}}
```

The greedy submission scores 94.1 on the same cases, which is the point — there
has to be something to beat.

## The pieces

| | |
| --- | --- |
| `BUILD` | one `arena_problem()` call: the config test, the tournament, the kit |
| `problem.textproto` | the whole problem: what to build, what to run, what to rank |
| `grader/grade_main.cc` | runs each case, checks the answer, writes the report |
| `cases/*.txt`, `*.best` | the instances and their known optima |
| `solutions/reference/` | an exact DP — the shape a submission takes |
| `solutions/greedy/` | a worse one, so the board has an order |

## The contract

A submission is one `cc_binary` named `solve` under `solutions/<id>/`. It takes
an instance path as its only argument and prints the indices of the items it
packs.

```
5 10        <- 5 items, capacity 10
6 5         <- value 6, weight 5
5 4
8 6
3 2
4 3
```

Score is the mean percentage of the known optimum across all cases. An
infeasible answer — over capacity, a repeated index, junk on stdout — scores
zero for that case, and so does a crash or a timeout.

## Writing a grader

`grade_main.cc` is ~150 lines and the only part worth copying. Two rules it
follows:

**Check feasibility yourself.** A submission is not a collaborator. The score
has to be something it cannot claim without earning — which is why the grader
recomputes the weight rather than trusting a total.

**A bad submission is not a broken run.** A crash, a hang or nonsense on stdout
scores zero for that case and the run continues. Only the grader failing — an
unreadable case file, say — exits non-zero. Otherwise one bad submission looks
like a broken problem.

It links two arena libraries, neither of which is required:
`@game_arena//game_arena/grader:report` writes the JSON, and
`@game_arena//game_arena/common/process` runs the submission under a timeout and
an address-space cap. A grader in any other language should just `printf` the
report:

```sh
printf '{"metrics": {"score": %s}}\n' "$score" > "$ARENA_REPORT"
```

## Running it, and what a participant gets

```sh
bazel run //:tournament -- --no_container        # coordinator + a local worker
bazel run //:kit -- --out=/srv/kits/bob --mint=bob --server=$(hostname):50051
```

The kit holds `solutions/reference/` and this README -- and not `grader/` or
`cases/`, because `kit_files` in `BUILD` does not name them: a participant
who can read the cases can special-case them. What a participant *can* do is
build a `solve` and submit it:

```sh
. ./arena.env
bazel run //:arena_cli -- submit --name="Greedy" --file=solutions/reference/solve.cc --wait
```

This directory is inside game-arena's git tree, which a worker cannot clone;
`scripts/new_problem.sh graded <dir>` copies it out as a repository of its own.

## A note on structured submissions

`submission.harness` here names `solve.cc` as its own `main_src`, because a
solution to this problem *is* a standalone program — there is no harness to
compile it into. That is the degenerate case of the harness field. The
[connect4](../connect4) example shows the interesting one, where the harness
owns a protocol and the submission is a single function.
