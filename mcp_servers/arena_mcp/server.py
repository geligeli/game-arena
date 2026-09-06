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

# The venv flow loads hand-generated stubs from _pb/; under `bazel run` the
# stubs come from //game_mcts/tournament_server/proto:arena_py instead, and
# _pb must stay off the path (resolve() follows runfiles symlinks back into
# the checkout, where _pb may hold stubs for a different protobuf runtime).
if "BUILD_WORKSPACE_DIRECTORY" not in os.environ:
    sys.path.insert(0, str(Path(__file__).resolve().parent / "_pb"))
try:
    from game_mcts.tournament_server.proto import arena_pb2, arena_pb2_grpc  # noqa: E402
except ImportError:  # bazel runfiles layout: flat stubs from :arena_py
    import arena_pb2  # type: ignore
    import arena_pb2_grpc  # type: ignore

# Under `bazel run`, BUILD_WORKSPACE_DIRECTORY points at the checkout.
_repo_root = os.environ.get("ARENA_MCP_REPO_ROOT") or os.environ.get(
    "BUILD_WORKSPACE_DIRECTORY"
)
REPO_ROOT = (
    Path(_repo_root) if _repo_root else Path(__file__).resolve().parents[2]
)
ARENA_TARGET = os.environ.get("ARENA_MCP_TARGET", "localhost:50051")
DEFAULT_AUTHOR = os.environ.get("ARENA_MCP_AUTHOR", "agent")
# Sent as the x-arena-token metadata header on writes. An arena with a client
# registry refuses Submit and Evaluate without it; one without a registry
# ignores it. Metadata rather than a request field, so it never lands in a
# stored submission or a log line.
ARENA_TOKEN = os.environ.get("ARENA_MCP_TOKEN", "")

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


def _auth() -> list[tuple[str, str]]:
    """Metadata for a write RPC. Empty when no token is configured."""
    return [("x-arena-token", ARENA_TOKEN)] if ARENA_TOKEN else []


def _rpc_error(error: grpc.RpcError) -> str:
    code = error.code()
    if code == grpc.StatusCode.UNAUTHENTICATED:
        return (
            f"ERROR: {error.details()}\n"
            "Set ARENA_MCP_TOKEN in this MCP server's environment. The arena's "
            "operator mints one with:\n"
            "  bazel run //game_mcts/tournament_server/tools:arena_admin -- "
            "mint --client_id=<you>"
        )
    if code == grpc.StatusCode.RESOURCE_EXHAUSTED:
        return (
            f"ERROR: over quota. {error.details()}\n"
            "Poll arena_job until it finishes, or pass cancel_running=True to "
            "replace it."
        )
    if code == grpc.StatusCode.UNAVAILABLE:
        return (
            f"ERROR: no arena at {ARENA_TARGET}. Start it with:\n"
            "  bazel run //game_mcts/tournament_server/server:problem_server -- "
            "--problem_config=game_mcts/tournament_server/problems/risk2.textproto "
            "--data_dir=tournament_data"
        )
    return f"ERROR: {error.details()}"


def _truncate(text: str, limit: int) -> str:
    if len(text) <= limit:
        return text
    return text[:limit] + f"\n... ({len(text) - limit} chars truncated)"


def _standing_row(standing, graded: bool = False) -> str:
    """One leaderboard row, rendered for whichever kind of problem this is."""
    candidate = standing.candidate
    if graded:
        # A measurement is not a property of the submission alone, so the host
        # that produced it belongs on the row.
        extra = f"{standing.runs:>4}r {standing.machine_class or '-':<12}"
    else:
        played = standing.wins + standing.draws + standing.losses
        extra = (
            f"{standing.wins:>3}/{standing.draws:>3}/{standing.losses:>3} "
            f"{played:>4}g"
        )
    return (
        f"{candidate.candidate_id:<28} {standing.score:9.3f} {extra} "
        f"{_STATUS.get(candidate.status, '?'):<12} "
        f"{candidate.author or '-':<12} {candidate.display_name}"
    )


def _standing_header(score_label: str, graded: bool = False) -> str:
    extra = (
        f"{'runs':>5} {'machine':<12}"
        if graded
        else f"{'W/D/L':>11} {'games':>5}"
    )
    return (
        f"{'candidate_id':<28} {(score_label or 'score'):>9} {extra} "
        f"{'status':<12} {'author':<12} name"
    )


@mcp.tool()
def arena_rules() -> str:
    """This server's problem, its limits and the workflow, in one call.

    Read this first. One server runs one problem, and it is the authority on
    what that problem is -- a README in the repo may describe a different one.
    """
    try:
        problem = _stub().GetProblem(
            arena_pb2.GetProblemRequest(), timeout=RPC_TIMEOUT_S
        )
    except grpc.RpcError as error:
        return _rpc_error(error)

    out = [
        f"PROBLEM  {problem.display_name or problem.problem_id}"
        f"  [{problem.problem_id}]",
        f"  scored by {problem.score_label}"
        + (
            f", {'lower' if problem.lower_is_better else 'higher'} is better"
            if problem.graded
            else " (play others and be rated)"
        ),
        f"  built against base_commit {problem.base_commit or '-'}",
        "",
    ]
    if problem.description:
        out += [problem.description.rstrip(), ""]

    out.append("SUBMITTING")
    if problem.files_submit_dir:
        out += [
            "  Either a list of files -- the server turns them into a patch"
            f" under\n    {problem.files_submit_dir}/<your-id>/, generating the"
            " BUILD:",
            '    arena_submit(display_name="My Bot", paths=["path/to/strategy.h"])',
            "",
            "  Or a unified diff against base_commit, for anything the file"
            " form cannot express:",
            '    arena_submit(display_name="My Bot", patch_path="my.diff")',
        ]
    else:
        out += [
            "  A unified diff against base_commit. This problem's solutions"
            " change existing\n  code, so a file list cannot express one:",
            '    arena_submit(display_name="Faster", patch_path="my.diff")',
        ]
    out += [
        "  Then poll arena_job(job_id).",
        "",
        "LIMITS",
        f"  patch       <= {problem.max_patch_bytes} bytes,"
        f" {problem.max_files} files, {problem.max_hunks} hunks",
    ]
    if problem.allow_paths:
        out.append(f"  may touch   {' '.join(problem.allow_paths)}")
    if problem.deny_paths:
        out.append(f"  may not     {' '.join(problem.deny_paths)}")
    out += [
        "",
        "LEARN FROM RIVALS",
        "  arena_candidates()                 who exists, and how they rank",
        "  arena_source(candidate_id)         what their patch touches",
        "  arena_source(candidate_id, path)   their code -- all of it is readable",
        "  Set parent_id when you build on someone, so lineage is recorded.",
        "",
        "MORE WORK",
        "  arena_evaluate(candidate_id, repeats=5)      measure again (graded)",
        '  arena_evaluate(candidate_id, opponent="ladder")  more games (match)',
    ]
    return "\n".join(out)



@mcp.tool()
def arena_submit(
    display_name: str,
    paths: list[str] | None = None,
    patch_path: str = "",
    entry_header: str = "",
    notes: str = "",
    parent_id: str = "",
    cancel_running: bool = False,
    game: str = "risk2",
    params: dict[str, str] | None = None,
    extra_deps: list[str] | None = None,
    author: str = "",
) -> str:
    """Submits a solution and queues its build plus a placement series.

    Two forms, and arena_rules() says which this problem takes:

    paths: files to submit, relative to the repo root or absolute. They are
    read from disk here, so you never paste code you just wrote. The server
    turns them into a patch under the problem's submission directory and
    generates the BUILD file.

    patch_path: a unified diff against the problem's base_commit, read from
    disk. The general form -- it can change anything the problem allows.

    entry_header: the file defining MakePolicy. Defaults to the sole .h/.hpp
    among paths. Only meaningful with `paths`.

    Returns the candidate id and job id. Poll arena_job(job_id) for the build.
    """
    if not paths and not patch_path:
        return "ERROR: pass either paths=[...] or patch_path=... (see arena_rules)"
    if paths and patch_path:
        return "ERROR: pass paths or patch_path, not both"

    request = arena_pb2.SubmitRequest(
        display_name=display_name,
        author=author or DEFAULT_AUTHOR,
        game=game,
        parent_id=parent_id,
        notes=notes,
        cancel_running=cancel_running,
    )
    if patch_path:
        path = Path(patch_path)
        if not path.is_absolute():
            path = REPO_ROOT / patch_path
        if not path.is_file():
            return f"ERROR: no such file: {patch_path}"
        request.patch = path.read_bytes()
        try:
            response = _stub().Submit(
                request, timeout=RPC_TIMEOUT_S, metadata=_auth()
            )
        except grpc.RpcError as error:
            return _rpc_error(error)
        return (
            f"candidate {response.candidate_id}\njob {response.job_id}\n"
            f'poll: arena_job("{response.job_id}")'
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
        response = _stub().Submit(
            request, timeout=RPC_TIMEOUT_S, metadata=_auth()
        )
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
        f"W/D/L {job.wins}/{job.draws}/{job.losses}  score {job.elo:.3f}",
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
        return "nothing has been scored yet"
    # The server says what its score column means; a graded problem's is a
    # metric name, not "elo".
    graded = response.score_label not in ("", "elo")
    rows = [_standing_header(response.score_label, graded)]
    rows.extend(_standing_row(row, graded) for row in response.rows)
    return "\n".join(rows)


@mcp.tool()
def arena_candidates(
    game: str = "", limit: int = 20, author: str = "", order: str = "best"
) -> str:
    """Every candidate, including ones still building or broken.

    order: "best" (default, whichever way this problem ranks) or "newest".
    Unlike arena_leaderboard this shows
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
            else arena_pb2.ListCandidatesRequest.BEST_FIRST
        ),
    )
    try:
        response = _stub().ListCandidates(request, timeout=RPC_TIMEOUT_S)
    except grpc.RpcError as error:
        return _rpc_error(error)
    if not response.candidates:
        return "no candidates yet"

    # This listing has no score_label of its own; a row carrying metrics is a
    # graded one.
    graded = any(standing.metrics for standing in response.candidates)
    rows = [_standing_header("score", graded)]
    for standing in response.candidates:
        row = _standing_row(standing, graded)
        if standing.candidate.parent_id:
            row += f"  <- {standing.candidate.parent_id}"
        rows.append(row)
    return "\n".join(rows)


@mcp.tool()
def arena_source(candidate_id: str, path: str = "") -> str:
    """Reads a rival's code. Any agent may read any candidate.

    Without path: the manifest, the paths the submission's patch touches, and
    the files that can be read back. With path: that file's contents.

    Paths are repo-relative, because a submission is a patch and a patch
    touches repo paths. Only files the patch *adds* can be read here -- a
    submission that modifies existing code changes lines that live in the repo,
    not in the arena, so read those from your own checkout at base_commit.
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
        ]
        if candidate.entry_header:
            lines.append(f"entry_header {candidate.entry_header}")
        if candidate.params:
            knobs = ",".join(f"{k}={v}" for k, v in sorted(candidate.params.items()))
            lines.append(f"params {knobs}")
        if candidate.extra_deps:
            lines.append(f"extra_deps {' '.join(candidate.extra_deps)}")
        if candidate.notes:
            lines.append(f"notes: {candidate.notes}")
        if candidate.touched_paths:
            lines.append("")
            lines.append("patch touches:")
            lines.extend(f"  {p}" for p in candidate.touched_paths)
        lines.append("")
        if candidate.file_paths:
            lines.append("readable here (files the patch adds):")
            lines.extend(f"  {p}" for p in candidate.file_paths)
        else:
            lines.append(
                "readable here: none -- this submission only modifies existing "
                "files. Read them from your own checkout at base_commit."
            )
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
def arena_evaluate(
    candidate_id: str,
    opponent: str = "",
    games: int = 0,
    repeats: int = 0,
    cancel_running: bool = False,
) -> str:
    """Queues more work for a candidate.

    On a match problem, pass `opponent`: "builtin:random" | "builtin:mcts" | a
    candidate id | "top" (the current leader) | "ladder" (a spread of rated
    rivals). `games` defaults to the problem's own.

    On a graded problem there is nothing to play against; pass `repeats` to
    measure again. More runs is a tighter number, not a better one.

    Sending the wrong one is an error rather than a silent default, so the
    problem tells you its shape the first time you guess wrong.
    """
    request = arena_pb2.EvaluateRequest(
        candidate_id=candidate_id, cancel_running=cancel_running
    )
    if repeats > 0 and not opponent:
        request.grade.repeats = repeats
    else:
        request.match.opponent = opponent or "ladder"
        if games > 0:
            request.match.games = games
    try:
        response = _stub().Evaluate(
            request, timeout=RPC_TIMEOUT_S, metadata=_auth()
        )
    except grpc.RpcError as error:
        return _rpc_error(error)
    return f"job {response.job_id} queued\npoll: arena_job(\"{response.job_id}\")"


if __name__ == "__main__":
    mcp.run()
