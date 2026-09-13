#!/usr/bin/env bash
# cpp_format.sh — lint / diff / fix C++ across a Bazel repo, or any target
# pattern, WITHOUT adding cpp_format_targets to your BUILD files.
#
# It runs the cpp_format aspect over the matching cc_* targets (each emits an
# edit-record file, parallel + cached), then merges every target's records into
# one repository-wide change with `cpp_format --aggregate`.
#
#   Usage: cpp_format.sh <check|diff|fix> [target-pattern]
#
#     check   exit 1 if any edit would be made (CI lint gate); writes nothing
#     diff    print the merged, git-apply-able unified patch
#     fix     apply the edits to your sources in place
#
#   target-pattern defaults to //... (the whole repo). Examples:
#     cpp_format.sh fix                 # fix the entire repo
#     cpp_format.sh diff //app/...      # preview just one package tree
#     cpp_format.sh check //lib:core    # gate a single target (+ its sources)
#
#   Tag a target `no-cpp-format` to exclude it.
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

mode="${1:-}"
pattern="${2:-//...}"
case "$mode" in
  check) agg=(--check) ;;
  diff)  agg=() ;;
  fix)   agg=(--apply) ;;
  *) echo "usage: $0 <check|diff|fix> [target-pattern]" >&2; exit 2 ;;
esac

# 1. Enumerate first-party cc_* targets under the pattern. This deliberately
#    excludes any cpp_format_targets() rule targets, so the aspect is applied to
#    each source target exactly once (two applications would collide on the
#    shared record file). `no-cpp-format`-tagged targets are skipped.
mapfile -t targets < <("$BAZEL" query \
  "kind('cc_(library|binary|test) rule', $pattern) except attr(tags, 'no-cpp-format', $pattern)" \
  2>/dev/null)
if [[ ${#targets[@]} -eq 0 ]]; then
  echo "cpp_format: no cc targets under $pattern" >&2
  exit 0
fi

# 2. Emit one edit-record file per target via the aspect.
"$BAZEL" build "${targets[@]}" \
  --aspects="$ASPECT" --output_groups=+cpp_format_edits >/dev/null

# 3. Resolve the cpp_format binary and the emitted record files.
"$BAZEL" build "$BIN_LABEL" >/dev/null 2>&1 || true
bin="$("$BAZEL" cquery --output=files "$BIN_LABEL" 2>/dev/null | tail -1)"
[[ -n "$bin" ]] || { echo "cpp_format: cannot locate $BIN_LABEL" >&2; exit 1; }
[[ "$bin" = /* ]] || bin="$("$BAZEL" info execution_root)/$bin"

bazel_bin="$("$BAZEL" info bazel-bin)"
workspace="$("$BAZEL" info workspace)"

# Record file paths are deterministic: //pkg:name -> <bazel-bin>/pkg/name.json.
# Header-/source-less targets emit nothing, so only keep files that exist.
records=()
for t in "${targets[@]}"; do
  rel="${t#//}"
  pkg="${rel%%:*}"
  name="${rel##*:}"
  f="$bazel_bin/$pkg/$name.cpp_format.json"
  [[ -f "$f" ]] && records+=("$f")
done
if [[ ${#records[@]} -eq 0 ]]; then
  echo "cpp_format: no records emitted for $pattern" >&2
  exit 0
fi

# 4. Merge every target's records into one repository-wide change.
exec "$bin" --aggregate "${agg[@]}" --root="$workspace" "${records[@]}"
