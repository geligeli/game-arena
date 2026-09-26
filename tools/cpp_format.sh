#!/usr/bin/env bash
# cpp_format.sh — lint / diff / fix C++ across a Bazel repo, or any target
# pattern, with nothing added to your BUILD files.  The same aspect machinery
# writes a compile_commands.json for your editor, builds the symbol index, and
# serves the repository in the code browser.
#
# It runs the cpp_format aspect over the matching cc_* targets (one action per
# source file emits that file's edit records -- parallel, cached, and per file,
# so editing one .cpp re-parses one file), then merges every record into one
# repository-wide change with `cpp_format --aggregate`.
#
#   Usage: cpp_format.sh <check|diff|fix|compile_commands|index|browse> [target-pattern] [flags]
#
#     check            exit 1 if any edit would be made (CI lint gate); writes nothing
#     diff             print the merged, git-apply-able unified patch
#     fix              apply the edits to your sources in place
#     compile_commands write compile_commands.json in the workspace root (for
#                      clangd & co.); compiles nothing and never runs cpp_format
#     index            build the symbol index -- every symbol's declarations,
#                      definitions and references across the matching targets,
#                      as byte ranges -- and write it to index.pb in the
#                      workspace root (a cpp_index.Index protobuf; read it with
#                      `cpp_format --dump-index [--lookup=<file>:<offset>]`)
#     browse           `index`, then serve the workspace in the code browser
#                      over it (http://127.0.0.1:8080/): every indexed token
#                      annotated, click through to definitions and references.
#                      Prints the browser binary and its command line first.
#
#   target-pattern defaults to //... (the whole repo). Any argument starting
#   with `-` is passed through: to `cpp_format --aggregate` by check/diff/fix --
#   the one worth knowing is --report-rename-conflicts, which lists every rename
#   the run declined (a count is printed either way -- a skipped rename is the
#   one outcome the diff cannot show) -- and to the server by browse (--port=N,
#   --address=A, --check to print the index stats and exit). Examples:
#     cpp_format.sh fix                 # fix the entire repo
#     cpp_format.sh diff //app/...      # preview just one package tree
#     cpp_format.sh check //lib:core    # gate a single target (+ its sources)
#     cpp_format.sh fix --report-rename-conflicts   # ... and say what it skipped
#     cpp_format.sh compile_commands    # refresh compile_commands.json
#     cpp_format.sh index //app/...     # index one package tree
#     cpp_format.sh browse              # index the entire repo and browse it
#     cpp_format.sh browse //app/... --port=9000
#
#   Tag a target `no-cpp-format` to exclude it from formatting (it still gets
#   compile_commands entries and is indexed); `no-cpp-index` excludes it from
#   the index.  COMPILE_COMMANDS_OUT and INDEX_OUT override the output paths.
#
#   The index is a snapshot: the server reads it once, at start.  After editing
#   sources, stop the server and run `browse` again -- only the translation
#   units that changed are re-parsed, and an unchanged index is not re-imported.
#
# This is the one file you keep locally (it runs outside Bazel). Point it at the
# aspect via CPP_FORMAT_ASPECT:
#   * imported by URL (archive_override):
#       @cpp_formatting//bazel/integration:cpp_format.bzl%cpp_format_aspect
#   * vendored into //third_party/cpp_format (the default below): no env needed.

set -euo pipefail

# --- configuration (override via env for URL import or a different vendor dir) -
ASPECT="${CPP_FORMAT_ASPECT:-@@cpp_formatting+//bazel/integration:cpp_format.bzl%cpp_format_aspect}"
BIN_LABEL="${CPP_FORMAT_BIN_LABEL:-@@cpp_formatting++cpp_format+cpp_format_bin//:cpp_format}"
# The code browser of the same release; fetched the first time `browse` runs.
BROWSER_LABEL="${CODE_BROWSER_LABEL:-@@cpp_formatting++cpp_format+code_browser_bin//:code_browser}"
BAZEL="${BAZEL:-bazel}"

# Merges the aspect's per-target `.compile_commands.jsonl` fragments into one
# compile_commands.json.  Each fragment holds one JSON object per source file
# with two placeholders that are only known here, at run time -- `directory` is
# the execution root (where every relative flag resolves) and `file` is the
# source's absolute workspace path (what an editor opens).
# Args: <exec root> <workspace> <output> <fragment>...
# A file listed by several targets keeps the first entry seen.
json_escape() { local s="$1" bs='\'; s="${s//"$bs"/"$bs$bs"}"; s="${s//\"/$bs\"}"; printf '%s' "$s"; }
merge_compile_commands() {
  local exec_root="$1" workspace="$2" out="$3"; shift 3
  local dir_json ws_json frag line key first=1
  dir_json="$(json_escape "$exec_root")"
  ws_json="$(json_escape "$workspace")"
  declare -A seen=()
  {
    printf '[\n'
    for frag in "$@"; do
      while IFS= read -r line; do
        [[ -n "$line" ]] || continue
        key="${line#*\"file\":\"}"; key="${key%%\"*}"
        [[ -z "${seen[$key]:-}" ]] || continue
        seen[$key]=1
        line="${line//\"__EXEC_ROOT__\"/"\"$dir_json\""}"
        line="${line//\"__WORKSPACE__\//"\"$ws_json/"}"
        [[ $first -eq 1 ]] || printf ',\n'
        first=0
        printf '  %s' "$line"
      done < "$frag"
    done
    printf '\n]\n'
  } > "$out.tmp"
  mv -f "$out.tmp" "$out"
}

# Sourced rather than run: only the functions above are wanted.  That is how
# //bazel/testdata:compile_commands_test gets at the merge -- this script drives
# Bazel, so nothing else in it can run inside a Bazel test.
if [[ "${BASH_SOURCE[0]}" != "$0" ]]; then
  return 0
fi

usage() {
  echo "usage: $0 <check|diff|fix|compile_commands|index|browse> [target-pattern] [--flag...]" >&2
  exit 2
}

mode="${1:-}"
case "$mode" in
  check) agg=(--check) ;;
  diff)  agg=() ;;
  fix)   agg=(--apply) ;;
  compile_commands|index|browse) agg=() ;;
  *) usage ;;
esac
shift || true

# The remaining arguments: at most one target pattern, plus flags -- handed to
# `cpp_format --aggregate` (--report-rename-conflicts being the useful one), or
# by `browse` to the server (--port=N, --check, ...).  compile_commands and
# index have nothing to hand them to and would silently drop them, so they
# refuse.
pattern=""
server_args=()
for arg in "$@"; do
  case "$arg" in
    -*)
      case "$mode" in
        compile_commands|index)
          echo "$0: $mode takes no flags (got '$arg')" >&2
          usage
          ;;
        browse) server_args+=("$arg") ;;
        *) agg+=("$arg") ;;
      esac
      ;;
    *)
      [[ -z "$pattern" ]] || { echo "$0: more than one target pattern ('$pattern', '$arg')" >&2; usage; }
      pattern="$arg"
      ;;
  esac
done
# As typed, for the instructions `browse` prints; then the default.
pattern_arg="${pattern:+ $pattern}"
pattern="${pattern:-//...}"
# The name to print for this script (a wrapper that execs it sets its own).
self="${CPP_FORMAT_SH_NAME:-$0}"

# The index aspect lives in the same .bzl as the formatting one, so its label is
# derived from ASPECT (baked in by `install`, or given via env); CPP_INDEX_ASPECT
# overrides it outright.
INDEX_ASPECT="${CPP_INDEX_ASPECT:-${ASPECT%\%*}%cpp_index_aspect}"

# 1. Enumerate first-party cc_* targets under the pattern: only those, so the
#    aspect is applied to each source target exactly once, at the top level
#    (two applications would collide on the shared record files).
#    `no-cpp-format`-tagged targets are skipped when formatting; a compilation
#    database and the index cover them too, and `no-cpp-index` is the index's
#    own opt-out.
query="kind('cc_(library|binary|test) rule', $pattern)"
case "$mode" in
  compile_commands) ;;
  index|browse) query="$query except attr(tags, 'no-cpp-index', $pattern)" ;;
  *) query="$query except attr(tags, 'no-cpp-format', $pattern)" ;;
esac
mapfile -t targets < <("$BAZEL" query "$query" 2>/dev/null)
if [[ ${#targets[@]} -eq 0 ]]; then
  echo "cpp_format: no cc targets under $pattern" >&2
  exit 0
fi

bazel_bin="$("$BAZEL" info bazel-bin)"
workspace="$("$BAZEL" info workspace)"
exec_root="$("$BAZEL" info execution_root)"

if [[ "$mode" == compile_commands ]]; then
  # Only the fragments (plus the generated headers they mention) are built:
  # `--output_groups=` without `+` replaces the default outputs, so nothing is
  # compiled and no edit-record action runs.  Each target's fragment is at a
  # deterministic path -- //pkg:name -> <bazel-bin>/pkg/name.compile_commands.jsonl;
  # source-less targets write none.
  "$BAZEL" build "${targets[@]}" \
    --aspects="$ASPECT" --output_groups=cpp_format_compile_commands >/dev/null
  frags=()
  for t in "${targets[@]}"; do
    rel="${t#//}"
    pkg="${rel%%:*}"
    name="${rel##*:}"
    frag="$bazel_bin/$pkg/$name.compile_commands.jsonl"
    [[ -f "$frag" ]] && frags+=("$frag")
  done
  out="${COMPILE_COMMANDS_OUT:-$workspace/compile_commands.json}"
  merge_compile_commands "$exec_root" "$workspace" "$out" "${frags[@]}"
  echo "cpp_format: wrote $out (${#frags[@]} targets)" >&2
  exit 0
fi

# 2. Emit one record file per source file via the aspect: an edit-record JSON
#    for formatting, an index unit for `index` and `browse`.  The two aspects
#    share the machinery and differ only in what each per-file action writes.
if [[ "$mode" == index || "$mode" == browse ]]; then
  aspect="$INDEX_ASPECT"; group=cpp_index; manifest_suffix=cpp_index.manifest
else
  aspect="$ASPECT"; group=cpp_format_edits; manifest_suffix=cpp_format.manifest
fi
"$BAZEL" build "${targets[@]}" \
  --aspects="$aspect" --output_groups="+$group" >/dev/null

# 3. Resolve the cpp_format binary and the emitted record files.
#
# Builds a binary's label and prints the path of its file.  The build log is
# kept back unless the build fails: for a prebuilt binary that is where the
# repository rule says what is missing (no release() tag, no asset for this
# host, a release that predates the code browser).
resolve_binary() {
  local label="$1" log path
  log="$(mktemp)"
  if ! "$BAZEL" build "$label" >"$log" 2>&1; then
    cat "$log" >&2
    rm -f "$log"
    echo "cpp_format: cannot build $label" >&2
    return 1
  fi
  rm -f "$log"
  path="$("$BAZEL" cquery --output=files "$label" 2>/dev/null | tail -1)"
  [[ -n "$path" ]] || { echo "cpp_format: cannot locate $label" >&2; return 1; }
  [[ "$path" = /* ]] || path="$exec_root/$path"
  printf '%s\n' "$path"
}
bin="$(resolve_binary "$BIN_LABEL")"

# Each target's manifest is at a deterministic path -- //pkg:name ->
# <bazel-bin>/pkg/name.cpp_format.manifest (or .cpp_index.manifest) -- and
# lists that target's per-file record files, exec-root relative.  It is read
# rather than the records directory globbed because Bazel never deletes the
# record of a source that was since removed from the target, and a stale
# record would apply stale edits (or keep stale occurrences in the index).
# Header-/source-less targets write no manifest, so only read the ones that
# exist.  The records go to the merger through a list file: a repository's
# worth of them does not fit on a command line.
list="$(mktemp)"
trap 'rm -f "$list"' EXIT
for t in "${targets[@]}"; do
  rel="${t#//}"
  pkg="${rel%%:*}"
  name="${rel##*:}"
  manifest="$bazel_bin/$pkg/$name.$manifest_suffix"
  [[ -f "$manifest" ]] || continue
  while IFS= read -r rec; do
    [[ -n "$rec" ]] && printf '%s\n' "$exec_root/$rec"
  done < "$manifest" >> "$list"
done
if [[ ! -s "$list" ]]; then
  echo "cpp_format: no records emitted for $pattern" >&2
  exit 0
fi

# 4. Merge every file's records: into one repository-wide change, or into one
#    index.
rc=0
if [[ "$mode" == index || "$mode" == browse ]]; then
  out="${INDEX_OUT:-$workspace/index.pb}"
  # Merged next to the destination and moved over it only when the bytes
  # differ.  The index is a pure function of its content, and the code browser
  # re-imports an index.pb that is newer than its database -- which for a
  # large repository is the slow part of starting it, and not worth paying
  # for an index that did not change.
  "$bin" --merge-index --output="$out.tmp" --records-from="$list" || rc=$?
  if [[ $rc -ne 0 ]]; then
    rm -f "$out.tmp"
    exit $rc
  fi
  if [[ -f "$out" ]] && cmp -s "$out.tmp" "$out"; then
    rm -f "$out.tmp"
    echo "cpp_format: $out is up to date" >&2
  else
    mv -f "$out.tmp" "$out"
    echo "cpp_format: wrote $out" >&2
  fi
  [[ "$mode" == index ]] && exit 0

  # 5. browse: serve the workspace over that index.  The checkout is the
  #    workspace; the index names files relative to the execution root, which
  #    is passed explicitly so that a --symlink_prefix hiding the bazel-out
  #    link does not matter.  --index imports index.pb into index.pb.sqlite
  #    when that is missing or older.
  browser="$(resolve_binary "$BROWSER_LABEL")"
  cmd=("$browser" --index="$out" --root="$workspace" --exec-root="$exec_root" "${server_args[@]}")
  {
    echo "cpp_format: code browser binary: $browser"
    printf 'cpp_format: command line:\n   '
    printf ' %q' "${cmd[@]}"
    printf '\n'
    echo "cpp_format: the index is a snapshot, read once when the server starts.  To update it"
    echo "  after editing sources, stop the server (Ctrl-C) and run this again:"
    echo "      $self browse$pattern_arg"
    echo "  Only the translation units that changed are re-parsed.  Or leave the server"
    echo "  up, refresh the index on the side, and restart with the command line above"
    echo "  (it re-imports $out when that is newer than its database):"
    echo "      $self index$pattern_arg"
  } >&2
  exec "${cmd[@]}"
fi
"$bin" --aggregate "${agg[@]}" --root="$workspace" --records-from="$list" || rc=$?
exit $rc
