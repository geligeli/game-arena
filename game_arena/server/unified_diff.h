#ifndef GAME_ARENA_GAME_ARENA_SERVER_UNIFIED_DIFF_H
#define GAME_ARENA_GAME_ARENA_SERVER_UNIFIED_DIFF_H

// Lenient about hunk bodies on purpose: only the worker applies, with real git.

#include <string>
#include <string_view>
#include <vector>

namespace tournament_arena {

struct PatchFile {
  // Repo-relative; old_path is empty when added, new_path when deleted.
  std::string old_path;
  std::string new_path;
  bool is_new = false;
  // Only for an added file: its contents, rebuilt from its '+' lines.
  std::string added_content;

  const std::string &path() const {
    return new_path.empty() ? old_path : new_path;
  }
};

struct Patch {
  std::vector<PatchFile> files;
  int total_hunks = 0;
};

// An empty diff is an error: a submission that changes nothing is not one.
bool ParseUnifiedDiff(std::string_view diff, Patch *out, std::string *error);

// Every path touched, in order, deduplicated.
std::vector<std::string> TouchedPaths(const Patch &patch);

struct NewFile {
  std::string path;  // repo-relative
  std::string content;
};

std::string MakeAddOnlyPatch(const std::vector<NewFile> &files);

// '*' within a segment, '**' across. No classes: they allow things by accident.
bool PathMatchesGlob(std::string_view path, std::string_view pattern);

}  // namespace tournament_arena

#endif  // GAME_ARENA_GAME_ARENA_SERVER_UNIFIED_DIFF_H
