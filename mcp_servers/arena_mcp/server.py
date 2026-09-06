"""Arena MCP server: submit strategies, read rivals, run matches.

A thin stdio -> gRPC shim over the tournament server's Arena service. All the
state lives there; this process holds nothing but a channel.

Two deliberate choices about token cost, which is what makes an agent loop on
this practical:

  * `arena_submit` takes *paths*, not file contents. The agent just wrote the
    strategy to disk; making it paste the code back would double the cost of
    every iteration.
  * Everything returned is compact fixed-width text with hard caps, and build
    failures come back as the extracted compiler errors only. The worker
    already trimmed the bazel log before it crossed the wire.

Run with the shared venv:  mcp_servers/.venv/bin/python server.py
"""

import os
import sys
from pathlib import Path

import grpc
from mcp.server.fastmcp import FastMCP

sys.path.insert(0, str(Path(__file__).resolve().parent / "_pb"))
from game_mcts.tournament_server import arena_pb2, arena_pb2_grpc  # noqa: E402

REPO_ROOT = Path(os.environ.get("ARENA_MCP_REPO_ROOT", Path(__file__).resolve().parents[2]))
ARENA_TARGET = os.environ.get("ARENA_MCP_TARGET", "localhost:50051")
DEFAULT_AUTHOR = os.environ.get("ARENA_MCP_AUTHOR", "agent")

# Hard caps so nothing the arena returns can flood the agent's context.
MAX_SOURCE_CHARS = 20_000
MAX_ERROR_CHARS = 4_000
RPC_TIMEOUT_S = 120.0

mcp = FastMCP("arena")

_STATUS = {
    arena_pb2.Candidate.PENDING: "pending",
    arena_pb2.Candidate.BUILDING: "building",
    arena_pb2.Candidate.READY: "ready",
    arena_pb2.Candidate.BUILD_FAILED: "build-failed",
    arena_pb2.Candidate.DISABLED: "disabled",
}
_JOB_STATE = {
    arena_pb2.Job.QUEUED: "queued",
    arena_pb2.Job.RUNNING: "running",
    arena_pb2.Job.DONE: "done",
    arena_pb2.Job.FAILED: "failed",
}


def _stub() -> arena_pb2_grpc.ArenaStub:
    # A channel per call: these are infrequent, and a cached one would go stale
    # across an arena restart in a way the agent would have to debug.
    return arena_pb2_grpc.ArenaStub(grpc.insecure_channel(ARENA_TARGET))


def _rpc_error(error: grpc.RpcError) -> str:
    code = error.code()
    if code == grpc.StatusCode.UNAVAILABLE:
        return (
            f"ERROR: no arena at {ARENA_TARGET}. Start it with:\n"
            "  bazel run //game_mcts/tournament_server:tournament_server -- "
            "--data_dir=tournament_data"
        )
    return f"ERROR: {error.details()}"


def _truncate(text: str, limit: int) -> str:
    if len(text) <= limit:
        return text
    return text[:limit] + f"\n... ({len(text) - limit} chars truncated)"


def _standing_row(standing) -> str:
    candidate = standing.candidate
    played = standing.wins + standing.draws + standing.losses
    return (
        f"{candidate.candidate_id:<28} {standing.elo:7.1f} "
        f"{standing.wins:>3}/{standing.draws:>3}/{standing.losses:>3} "
        f"{played:>4}g {_STATUS.get(candidate.status, '?'):<12} "
        f"{candidate.author or '-':<12} {candidate.display_name}"
    )


_STANDING_HEADER = (
    f"{'candidate_id':<28} {'elo':>7} {'W/D/L':>11} {'games':>5} "
    f"{'status':<12} {'author':<12} name"
)


@mcp.tool()
def arena_rules() -> str:
    """The candidate contract, limits and workflow, in one call.

    Read this before writing a strategy: it replaces reading the READMEs and
    the harness headers.
    """
    return f"""ARENA: write a Risk strategy, have it built and rated against others.

THE CONTRACT
  A candidate is one header that includes candidate_api.h and defines exactly:

    auto MakePolicy(const candidate::Params &params) -> candidate::policy_t;

  policy_t is mcts::tournament::AnyPolicy<game_t>, so you may return:
    - tournament_broker::MctsPolicy<game_t, YourProposer, YourRollout>{{...}}
    - any callable (game, gen) -> mcts::tournament::PolicyDecision<game_t>
  Params carries key=value knobs; every getter falls back rather than throwing.

  Everything else -- connecting, the handshake, (de)serialisation, reporting --
  is supplied. You never write networking code.

START FROM
  {REPO_ROOT}/game_mcts/tournament_server/candidates/dev/strategy.h
  It is the stock Risk MCTS bot plus a commented custom-proposer skeleton.

ITERATE LOCALLY (no submission needed; play the live arena)
  bazel run //game_mcts/tournament_server/candidate:dev_bot -- \\
      --name=me-dev --server={ARENA_TARGET} --opponent=builtin:mcts --games=5

SUBMIT (pass paths, not contents -- the arena reads them off disk)
  arena_submit(display_name="My Bot", paths=["game_mcts/.../candidates/dev/strategy.h"])
  Then poll arena_job(job_id). A new candidate is automatically placed against
  builtin:random and builtin:mcts.

LEARN FROM RIVALS
  arena_candidates()                 who exists, and how they rank
  arena_source(candidate_id)         their file list
  arena_source(candidate_id, path)   their actual code -- all of it is readable
  Set parent_id when you build on someone, so lineage is recorded.

LIMITS
  files       <= 32 per submission, 512 KiB each, 2 MiB total
  extensions  .h .hpp .cc .cpp .inl only; relative paths, no ".."
  deps        //game_mcts/cpp/mcts:*, //game_mcts/cpp/risk:*,
              //game_mcts/cpp/risk/strategies:*, @abseil-cpp//
  games       <= 200 per challenge

TWO TRAPS THAT COST REAL TIME
  - A proposer's support_size() must mirror sample()'s branches exactly, or
    DedupSampler asserts. See game_mcts/cpp/mcts/game_traits.h.
  - An attack-averse proposer stalls rollouts to the move cap: games take
    minutes and come back as draws. Always sanity-check against builtin:random.
"""


@mcp.tool()
def arena_submit(
    display_name: str,
    paths: list[str],
    entry_header: str = "",
    notes: str = "",
    parent_id: str = "",
    game: str = "risk2",
    params: dict[str, str] | None = None,
    extra_deps: list[str] | None = None,
    author: str = "",
) -> str:
    """Submits a strategy and queues its build plus a placement series.

    paths: files to submit, relative to the repo root or absolute. They are
    read from disk here, so you never paste code you just wrote. Each file is
    stored under the candidate's own directory using its basename, unless the
    path is already relative to a candidate directory.

    entry_header: the file defining MakePolicy. Defaults to the sole .h/.hpp
    among paths.

    Returns the candidate id and job id. Poll arena_job(job_id) for the build.
    """
    if not paths:
        return "ERROR: paths is required (the files making up the strategy)"

    request = arena_pb2.SubmitRequest(
        display_name=display_name,
        author=author or DEFAULT_AUTHOR,
        game=game,
        parent_id=parent_id,
        notes=notes,
    )
    headers = []
    for raw in paths:
        path = Path(raw)
        if not path.is_absolute():
            path = REPO_ROOT / raw
        if not path.is_file():
            return f"ERROR: no such file: {raw}"
        try:
            content = path.read_bytes()
        except OSError as error:
            return f"ERROR: cannot read {raw}: {error}"
        # Flattened to the basename: a submission is its own directory, so the
        # sender's layout above it is irrelevant and only invites "..".
        request.files.add(path=path.name, content=content)
        if path.suffix in (".h", ".hpp"):
            headers.append(path.name)

    if entry_header:
        request.entry_header = Path(entry_header).name
    elif len(headers) == 1:
        request.entry_header = headers[0]
    else:
        return (
            "ERROR: entry_header is required when the submission has "
            f"{len(headers)} headers (candidates: {', '.join(headers) or 'none'})"
        )

    for key, value in (params or {}).items():
        request.params[key] = str(value)
    for dep in extra_deps or []:
        request.extra_deps.append(dep)

    try:
        response = _stub().Submit(request, timeout=RPC_TIMEOUT_S)
    except grpc.RpcError as error:
        return _rpc_error(error)

    return (
        f"submitted {response.candidate_id}\n"
        f"job {response.job_id} queued (build + placement)\n"
        f"poll: arena_job(\"{response.job_id}\")"
    )


@mcp.tool()
def arena_job(job_id: str) -> str:
    """Status of a build/match job. On failure returns the compiler errors only."""
    try:
        job = _stub().GetJob(
            arena_pb2.GetJobRequest(job_id=job_id), timeout=RPC_TIMEOUT_S
        )
    except grpc.RpcError as error:
        return _rpc_error(error)

    lines = [
        f"job {job.job_id} [{_JOB_STATE.get(job.state, '?')}] "
        f"candidate {job.candidate_id}",
        f"games {job.games_played}/{job.games_requested}  "
        f"W/D/L {job.wins}/{job.draws}/{job.losses}  elo {job.elo:.1f}",
    ]
    if job.error:
        lines.append("")
        lines.append(_truncate(job.error, MAX_ERROR_CHARS))
    return "\n".join(lines)


@mcp.tool()
def arena_leaderboard(game: str = "risk2", limit: int = 20) -> str:
    """Current standings: who is winning, and by how much."""
    try:
        response = _stub().Leaderboard(
            arena_pb2.LeaderboardRequest(game=game, limit=limit),
            timeout=RPC_TIMEOUT_S,
        )
    except grpc.RpcError as error:
        return _rpc_error(error)
    if not response.rows:
        return f"no rated candidates yet for {game}"
    rows = [_STANDING_HEADER]
    rows.extend(_standing_row(row) for row in response.rows)
    return "\n".join(rows)


@mcp.tool()
def arena_candidates(
    game: str = "risk2", limit: int = 20, author: str = "", order: str = "elo"
) -> str:
    """Every candidate, including ones still building or broken.

    order: "elo" (default) or "newest". Unlike arena_leaderboard this shows
    pending and failed candidates too, which is what you want when looking for
    something to improve on.
    """
    request = arena_pb2.ListCandidatesRequest(
        game=game,
        author=author,
        limit=limit,
        order=(
            arena_pb2.ListCandidatesRequest.NEWEST
            if order == "newest"
            else arena_pb2.ListCandidatesRequest.ELO_DESC
        ),
    )
    try:
        response = _stub().ListCandidates(request, timeout=RPC_TIMEOUT_S)
    except grpc.RpcError as error:
        return _rpc_error(error)
    if not response.candidates:
        return f"no candidates yet for {game}"

    rows = [_STANDING_HEADER]
    for standing in response.candidates:
        row = _standing_row(standing)
        if standing.candidate.parent_id:
            row += f"  <- {standing.candidate.parent_id}"
        rows.append(row)
    return "\n".join(rows)


@mcp.tool()
def arena_source(candidate_id: str, path: str = "") -> str:
    """Reads a rival's code. Any agent may read any candidate.

    Without path: the manifest and file list. With path: that file's contents.
    """
    stub = _stub()
    if not path:
        try:
            candidate = stub.GetCandidate(
                arena_pb2.GetCandidateRequest(candidate_id=candidate_id),
                timeout=RPC_TIMEOUT_S,
            )
        except grpc.RpcError as error:
            return _rpc_error(error)
        lines = [
            f"{candidate.candidate_id}  \"{candidate.display_name}\"",
            f"author {candidate.author or '-'}  game {candidate.game}  "
            f"status {_STATUS.get(candidate.status, '?')}",
            f"base_commit {candidate.base_commit or '-'}  "
            f"parent {candidate.parent_id or '-'}",
            f"entry_header {candidate.entry_header}",
        ]
        if candidate.params:
            knobs = ",".join(f"{k}={v}" for k, v in sorted(candidate.params.items()))
            lines.append(f"params {knobs}")
        if candidate.extra_deps:
            lines.append(f"extra_deps {' '.join(candidate.extra_deps)}")
        if candidate.notes:
            lines.append(f"notes: {candidate.notes}")
        lines.append("")
        lines.append("files:")
        lines.extend(f"  {p}" for p in candidate.file_paths)
        if candidate.build_error:
            lines.append("")
            lines.append("build error:")
            lines.append(_truncate(candidate.build_error, MAX_ERROR_CHARS))
        return "\n".join(lines)

    try:
        source = stub.GetSource(
            arena_pb2.GetSourceRequest(candidate_id=candidate_id, path=path),
            timeout=RPC_TIMEOUT_S,
        )
    except grpc.RpcError as error:
        return _rpc_error(error)
    return _truncate(source.content.decode("utf-8", errors="replace"),
                     MAX_SOURCE_CHARS)


@mcp.tool()
def arena_challenge(candidate_id: str, opponent: str = "ladder", games: int = 10) -> str:
    """Queues more games for a candidate.

    opponent: "builtin:random" | "builtin:mcts" | a candidate id | "top" (the
    current leader) | "ladder" (a spread of rated rivals).
    """
    try:
        response = _stub().Challenge(
            arena_pb2.ChallengeRequest(
                candidate_id=candidate_id, opponent=opponent, games=games
            ),
            timeout=RPC_TIMEOUT_S,
        )
    except grpc.RpcError as error:
        return _rpc_error(error)
    return f"job {response.job_id} queued\npoll: arena_job(\"{response.job_id}\")"


if __name__ == "__main__":
    mcp.run()
