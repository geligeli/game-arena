#!/usr/bin/env bash
# cpp_format.sh — lint / diff / fix C++ across a Bazel repo, or any target
# pattern, WITHOUT adding cpp_format_targets to your BUILD files.  It also
# writes a compile_commands.json for your editor from the same aspect.
#
# It runs the cpp_format aspect over the matching cc_* targets (one action per
# source file emits that file's edit records -- parallel, cached, and per file,
# so editing one .cpp re-parses one file), then merges every record into one
# repository-wide change with `cpp_format --aggregate`.
#
#   Usage: cpp_format.sh <check|diff|fix|compile_commands|index> [target-pattern] [flags]
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
#
#   target-pattern defaults to //... (the whole repo). Any argument starting
#   with `-` is passed through to `cpp_format --aggregate`; the one worth
#   knowing is --report-rename-conflicts, which lists every rename the run
#   declined (a count is printed either way -- a skipped rename is the one
#   outcome the diff cannot show). Examples:
#     cpp_format.sh fix                 # fix the entire repo
#     cpp_format.sh diff //app/...      # preview just one package tree
#     cpp_format.sh check //lib:core    # gate a single target (+ its sources)
#     cpp_format.sh fix --report-rename-conflicts   # ... and say what it skipped
#     cpp_format.sh compile_commands    # refresh compile_commands.json
#     cpp_format.sh index //app/...     # index one package tree
#
#   Tag a target `no-cpp-format` to exclude it from formatting (it still gets
#   compile_commands entries and is indexed); `no-cpp-index` excludes it from
#   the index.  COMPILE_COMMANDS_OUT and INDEX_OUT override the output paths.
#
# This is the one file you keep locally (it runs outside Bazel). Point it at the
# aspect via CPP_FORMAT_ASPECT:
#   * imported by URL (archive_override):
#       @cpp_formatting//bazel/integration:cpp_format.bzl%cpp_format_aspect
#   * vendored into //third_party/cpp_format (the default below): no env needed.

set -euo pipefail

# --- configuration (override via env for URL import or a different vendor dir) -
ASPECT="${CPP_FORMAT_ASPECT:-@@cpp_formatting+//bazel/integration:cpp_format.bzl%cpp_format_aspect}"
BIN_LABEL="${CPP_FORMAT_BIN_LABEL:-@cpp_format_bin//:cpp_format}"
BAZEL="${BAZEL:-bazel}"

usage() {
  echo "usage: $0 <check|diff|fix|compile_commands|index> [target-pattern] [--aggregate-flag...]" >&2
  exit 2
}

mode="${1:-}"
case "$mode" in
  check) agg=(--check) ;;
  diff)  agg=() ;;
  fix)   agg=(--apply) ;;
  compile_commands|index) agg=() ;;
  *) usage ;;
esac
shift || true

# The remaining arguments: at most one target pattern, plus any flags to hand
# to `cpp_format --aggregate` (--report-rename-conflicts being the useful one).
# The two modes that do not aggregate would silently drop them, so they refuse.
pattern=""
for arg in "$@"; do
  case "$arg" in
    -*)
      if [[ "$mode" == compile_commands || "$mode" == index ]]; then
        echo "$0: $mode takes no flags (got '$arg')" >&2
        usage
      fi
      agg+=("$arg")
      ;;
    *)
      [[ -z "$pattern" ]] || { echo "$0: more than one target pattern ('$pattern', '$arg')" >&2; usage; }
      pattern="$arg"
      ;;
  esac
done
pattern="${pattern:-//...}"

# The index aspect lives in the same .bzl as the formatting one, so its label is
# derived from ASPECT (baked in by `install`, or given via env); CPP_INDEX_ASPECT
# overrides it outright.
INDEX_ASPECT="${CPP_INDEX_ASPECT:-${ASPECT%\%*}%cpp_index_aspect}"

# The same merge the `<name>.compile_commands` run target performs (kept in
# sync with _MERGE_COMPILE_COMMANDS_SNIPPET in cpp_format.bzl): each target's
# fragment holds one JSON object per source file with two placeholders --
# `directory` is the execution root (where every relative flag resolves) and
# `file` is the source's absolute workspace path (what an editor opens).
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

# 1. Enumerate first-party cc_* targets under the pattern. This deliberately
#    excludes any cpp_format_targets() rule targets, so the aspect is applied to
#    each source target exactly once (two applications would collide on the
#    shared record files). `no-cpp-format`-tagged targets are skipped when
#    formatting; a compilation database and the index cover them too, and
#    `no-cpp-index` is the index's own opt-out.
query="kind('cc_(library|binary|test) rule', $pattern)"
case "$mode" in
  compile_commands) ;;
  index) query="$query except attr(tags, 'no-cpp-index', $pattern)" ;;
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
#    for formatting, an index unit for `index`.  The two aspects share the
#    machinery and differ only in what each per-file action writes.
if [[ "$mode" == index ]]; then
  aspect="$INDEX_ASPECT"; group=cpp_index; manifest_suffix=cpp_index.manifest
else
  aspect="$ASPECT"; group=cpp_format_edits; manifest_suffix=cpp_format.manifest
fi
"$BAZEL" build "${targets[@]}" \
  --aspects="$aspect" --output_groups="+$group" >/dev/null

# 3. Resolve the cpp_format binary and the emitted record files.
"$BAZEL" build "$BIN_LABEL" >/dev/null 2>&1 || true
bin="$("$BAZEL" cquery --output=files "$BIN_LABEL" 2>/dev/null | tail -1)"
[[ -n "$bin" ]] || { echo "cpp_format: cannot locate $BIN_LABEL" >&2; exit 1; }
[[ "$bin" = /* ]] || bin="$exec_root/$bin"

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
if [[ "$mode" == index ]]; then
  out="${INDEX_OUT:-$workspace/index.pb}"
  "$bin" --merge-index --output="$out" --records-from="$list" || rc=$?
  [[ $rc -eq 0 ]] && echo "cpp_format: wrote $out" >&2
  exit $rc
fi
"$bin" --aggregate "${agg[@]}" --root="$workspace" --records-from="$list" || rc=$?
exit $rc
