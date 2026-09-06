#ifndef GAME_ARENA_GAME_ARENA_SERVER_UNIFIED_DIFF_H
#define GAME_ARENA_GAME_ARENA_SERVER_UNIFIED_DIFF_H

// Reading and writing unified diffs, on the coordinator.
//
// A submission is a patch, and the coordinator has to know three things about
// one before it stores it: which paths it touches (to check against the
// problem's policy), how big it is (to bound it), and what a newly added file
// contains (so the leaderboard can show source without a checkout).
//
// Deliberately a header parser, not a patch applier. The coordinator never
// applies anything -- that is the worker's job, in a container, with real git.
// Parsing here answers "may this be stored and dispatched"; a patch that parses
// but does not apply comes back as an ordinary order error, which is the right
// place for it because only the worker has the tree to apply it to.
//
// Being lenient about hunk bodies is intentional for the same reason: this is a
// gate on paths and size, and pretending to validate diff semantics the applier
// will re-check anyway would just be a second, worse implementation of git.

#include <string>
#include <string_view>
#include <vector>

namespace tournament_arena {

struct PatchFile {
  // Repo-relative. |old_path| is empty for an added file, |new_path| for a
  // deleted one; a rename has both and they differ.
  std::string old_path;
  std::string new_path;
  bool is_new = false;
  bool is_delete = false;
  int hunks = 0;
  // The added file's contents, rebuilt from its '+' lines. Only populated for
  // an added file, so the leaderboard can show a submission's source without
  // checking anything out.
  std::string added_content;

  // The path this entry is about, for policy checks and display: the new path
  // when there is one, else the old.
  auto path() const -> const std::string & {
    return new_path.empty() ? old_path : new_path;
  }
};

struct Patch {
  std::vector<PatchFile> files;
  int total_hunks = 0;
};

// Parses a unified diff. Returns false with *error set when a header is
// malformed or a path is unusable; an empty diff is an error, since a
// submission that changes nothing cannot be evaluated.
auto ParseUnifiedDiff(std::string_view diff, Patch *out,
                      std::string *error) -> bool;

// Every path |diff| touches, in order, deduplicated. Convenience over
// ParseUnifiedDiff for callers that only need the paths.
auto TouchedPaths(const Patch &patch) -> std::vector<std::string>;

// One file of an add-only patch.
struct NewFile {
  std::string path;  // repo-relative
  std::string content;
};

// Renders |files| as a `git apply`-able add-only unified diff. This is what
// turns the structured submit form (a list of files) into the one thing the
// worker knows how to handle, so there is a single execution path rather than
// two.
auto MakeAddOnlyPatch(const std::vector<NewFile> &files) -> std::string;

// True when |path| matches |pattern|, where '*' matches within one path
// segment and '**' matches across segments. No character classes: a submission
// policy is read by whoever operates the problem, and glob subtleties there
// are a way to allow something by accident.
auto PathMatchesGlob(std::string_view path, std::string_view pattern) -> bool;

}  // namespace tournament_arena

#endif  // GAME_ARENA_GAME_ARENA_SERVER_UNIFIED_DIFF_H
