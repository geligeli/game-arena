#!/bin/bash
# Scaffolds a new problem repository from one of the worked examples.
#
#   scripts/new_problem.sh match  /path/to/my_game   [--id=my_game]
#   scripts/new_problem.sh graded /path/to/my_bench  [--id=my_bench]
#
# Copies examples/connect4 (match) or examples/knapsack (graded), renames the
# module and the problem id, pins game_arena to this checkout's HEAD by
# git_override, and makes the first commit. An uncommitted .bazelrc.local
# points bazel at this checkout while both are being worked on.
set -euo pipefail

usage() {
  echo "usage: $0 <match|graded> <dir> [--id=<problem_id>]" >&2
  exit 2
}

[[ $# -ge 2 ]] || usage
shape="$1"
dest="$2"
shift 2
id=""
for arg in "$@"; do
  case "${arg}" in
    --id=*) id="${arg#--id=}" ;;
    *) usage ;;
  esac
done

case "${shape}" in
  match) example="connect4" ;;
  graded) example="knapsack" ;;
  *) usage ;;
esac

arena="$(cd "$(dirname "$0")/.." && pwd)"
src="${arena}/examples/${example}"
[[ -z "${id}" ]] && id="$(basename "${dest}")"
if ! [[ "${id}" =~ ^[a-z0-9][a-z0-9_-]{0,63}$ ]]; then
  echo "problem id '${id}' must be 1-64 chars, [a-z0-9] then [a-z0-9_-]" >&2
  exit 2
fi
if [[ -e "${dest}" ]] && [[ -n "$(ls -A "${dest}")" ]]; then
  echo "${dest} exists and is not empty" >&2
  exit 1
fi

remote="$(git -C "${arena}" remote get-url origin 2>/dev/null || echo "")"
commit="$(git -C "${arena}" rev-parse HEAD)"

mkdir -p "${dest}"
# Sources only: no bazel-* symlinks, no local state. The lockfile comes along
# and stays committed. The first local build updates it for the override
# below; commit that too.
tar -C "${src}" --exclude='./bazel-*' --exclude='./.arena' -cf - . \
    | tar -C "${dest}" -xf -

# Module name and problem id. The game itself keeps its name: it is the rules,
# not the problem, and renaming it would mean editing C++ the author will
# replace anyway.
sed -i -e "s/^    name = \"${example}_problem\",/    name = \"${id}_problem\",/" \
    "${dest}/MODULE.bazel"
sed -i -e "s/^problem_id: \"${example}\"/problem_id: \"${id}\"/" \
       -e "s/^display_name: \".*\"/display_name: \"${id}\"/" \
       -e "s#/${example}-sandbox:#/${id}-sandbox:#" \
    "${dest}/problem.textproto"
sed -i "s/${example}/${id}/g" "${dest}/deploy.sh"

# Pin game_arena. local_path_override in the examples is relative to
# game-arena's own tree, which this repo is not in. A commit the remote has is
# pinned by git_override, which any clone can fetch; one that exists only here
# is pointed at by absolute path, and said so, because nobody cloning this
# repo elsewhere could fetch it.
pin="path"
if [[ -n "${remote}" ]] && git -C "${arena}" branch -r --contains "${commit}" 2>/dev/null | grep -q .; then
  pin="git"
fi
python3 - "${dest}/MODULE.bazel" "${pin}" "${remote}" "${commit}" "${arena}" <<'PY'
import re, sys
path, pin, remote, commit, arena = sys.argv[1:]
text = open(path).read()
if pin == "git":
    override = (
        'git_override(\n'
        '    module_name = "game_arena",\n'
        f'    remote = "{remote}",\n'
        f'    commit = "{commit}",\n'
        ')\n'
    )
else:
    override = (
        '# game-arena commit ' + commit + ' is not on a remote yet; pinned by\n'
        '# path. Replace with git_override(remote, commit) once it is pushed.\n'
        'local_path_override(\n'
        '    module_name = "game_arena",\n'
        f'    path = "{arena}",\n'
        ')\n'
    )
new, n = re.subn(r'local_path_override\(\s*module_name = "game_arena",[^)]*\)\n',
                 override, text)
if n != 1:
    sys.exit("expected exactly one local_path_override(game_arena) in MODULE.bazel")
open(path, "w").write(new)
PY

grep -q 'try-import %workspace%/.bazelrc.local' "${dest}/.bazelrc" 2>/dev/null || \
  printf '\n# Machine-local overrides, uncommitted.\ntry-import %%workspace%%/.bazelrc.local\n' \
    >> "${dest}/.bazelrc"
cat > "${dest}/.bazelrc.local" <<RC
# Uncommitted. Build against the game-arena checkout this problem was
# scaffolded from instead of the commit MODULE.bazel pins, while both change.
common --override_module=game_arena=${arena}
RC
cat > "${dest}/.gitignore" <<GI
bazel-*
.bazelrc.local
GI

git -C "${dest}" init -q
git -C "${dest}" add -A
# With no identity configured, commit as the scaffolder rather than fail.
git -C "${dest}" \
    -c user.name="$(git config user.name || echo new_problem.sh)" \
    -c user.email="$(git config user.email || echo new_problem@game-arena)" \
    commit -q -m "Scaffold ${id} from game-arena's ${example} example"

cat <<MSG
Created ${dest} (problem_id ${id}, from examples/${example}).

MSG
if [[ "${pin}" == "git" ]]; then
  cat <<MSG
game_arena is pinned to ${commit} of ${remote} in MODULE.bazel;
.bazelrc.local (uncommitted) overrides it with ${arena}.
MSG
else
  cat <<MSG
WARNING: game-arena's HEAD (${commit}) is not on a remote, so MODULE.bazel
pins game_arena by path (${arena}). Nobody on another host can build that;
push game-arena and switch to git_override before sharing this repo.
MSG
fi
cat <<MSG

Next:
  cd ${dest}
  bazel test //...                                  # the rules and the config
  bazel run //:play                                 # a local arena + your kit
  bazel run //:kit -- --out=/tmp/kit --mint=me      # what a participant gets
  bazel build //:sandbox_image                      # the sandbox image: this tree

Every submission is built and run in a container: sandbox.image, which play
makes for you. A package you add exports its files (a "tree" filegroup) and is
named in arena_problem(tree = ...), or the sandbox does not have it.
./deploy.sh pushes the kit image to the registry it names, runs the
tournament on this host, and opens a participant's shell.
MSG
