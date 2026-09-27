#include "game_arena/server/unified_diff.h"

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "absl/strings/str_cat.h"
#include "absl/strings/str_split.h"
#include "absl/strings/strip.h"

namespace tournament_arena {

namespace {

// Splits on '\n', keeping empty lines. A trailing newline does not produce a
// final empty line, so a diff ending in "\n" has no phantom entry.
std::vector<std::string_view> SplitLines(std::string_view text) {
  std::vector<std::string_view> lines = absl::StrSplit(text, '\n');
  if (lines.back().empty()) {
    lines.pop_back();
  }
  return lines;
}

// Strips the "a/" or "b/" git prepends, and trims the trailing tab-timestamp a
// plain `diff -u` leaves behind.
std::string CleanPath(std::string_view raw) {
  const std::size_t tab = raw.find('\t');
  if (tab != std::string_view::npos) {
    raw = raw.substr(0, tab);
  }
  if (raw == "/dev/null") {
    return {};
  }
  if (!absl::ConsumePrefix(&raw, "a/")) {
    absl::ConsumePrefix(&raw, "b/");
  }
  return std::string(raw);
}

// The same rules the store applies to any path it writes under: relative, no
// '..', no absolute escape. Checked here because a patch header is the one
// place a path arrives as free text.
bool IsUsablePath(const std::string &path, std::string *error) {
  if (path.empty()) {
    *error = "a patch entry has an empty path";
    return false;
  }
  if (path.front() == '/') {
    *error =
        absl::StrCat("path '", path, "' must be relative to the repo root");
    return false;
  }
  for (const std::string_view part : absl::StrSplit(path, '/')) {
    if (part == ".." || part == ".") {
      *error =
          absl::StrCat("path '", path, "' contains a '", part, "' component");
      return false;
    }
  }
  return true;
}

}  // namespace

bool ParseUnifiedDiff(std::string_view diff, Patch *out, std::string *error) {
  *out = Patch{};
  const std::vector<std::string_view> lines = SplitLines(diff);

  PatchFile current;
  bool in_file = false;
  // Whether the hunk being read belongs to a newly added file, in which case
  // its '+' lines are the file's contents.
  bool collecting = false;

  const auto flush = [&] {
    if (in_file) {
      out->files.push_back(std::move(current));
      current = PatchFile{};
    }
    in_file = false;
    collecting = false;
  };

  for (const std::string_view line : lines) {
    if (line.starts_with("diff --git ")) {
      flush();
      in_file = true;
      continue;
    }
    if (line.starts_with("new file mode")) {
      current.is_new = true;
      continue;
    }
    if (line.starts_with("--- ")) {
      // A bare `diff -u` has no "diff --git" line, so the ---/+++ pair is what
      // starts a file.
      in_file = true;
      current.old_path = CleanPath(line.substr(4));
      if (current.old_path.empty()) {
        current.is_new = true;
      }
      continue;
    }
    if (line.starts_with("+++ ")) {
      current.new_path = CleanPath(line.substr(4));
      continue;
    }
    if (line.starts_with("@@")) {
      if (!in_file) {
        *error = "a hunk appears before any file header";
        return false;
      }
      ++out->total_hunks;
      collecting = current.is_new;
      continue;
    }
    if (collecting && line.starts_with("+")) {
      current.added_content.append(line.substr(1));
      current.added_content.push_back('\n');
      continue;
    }
    if (collecting && line.starts_with("\\ No newline at end of file")) {
      // The '+' line before this one did end the file, so undo the newline
      // this parser added for it.
      if (!current.added_content.empty()) {
        current.added_content.pop_back();
      }
      continue;
    }
  }
  flush();

  if (out->files.empty()) {
    *error = "the patch is empty: nothing to build";
    return false;
  }
  for (const PatchFile &file : out->files) {
    if (!file.old_path.empty() && !IsUsablePath(file.old_path, error)) {
      return false;
    }
    if (!file.new_path.empty() && !IsUsablePath(file.new_path, error)) {
      return false;
    }
    if (file.old_path.empty() && file.new_path.empty()) {
      *error = "a patch entry names no file on either side";
      return false;
    }
  }
  return true;
}

std::vector<std::string> TouchedPaths(const Patch &patch) {
  std::vector<std::string> paths;
  for (const PatchFile &file : patch.files) {
    for (const std::string &path : {file.old_path, file.new_path}) {
      if (!path.empty() && !std::ranges::contains(paths, path)) {
        paths.push_back(path);
      }
    }
  }
  return paths;
}

std::string MakeAddOnlyPatch(const std::vector<NewFile> &files) {
  std::string diff;
  for (const NewFile &file : files) {
    const std::vector<std::string_view> lines = SplitLines(file.content);
    const bool ends_with_newline =
        !file.content.empty() && file.content.back() == '\n';

    absl::StrAppend(&diff, "diff --git a/", file.path, " b/", file.path, "\n");
    absl::StrAppend(&diff, "new file mode 100644\n");
    absl::StrAppend(&diff, "--- /dev/null\n");
    absl::StrAppend(&diff, "+++ b/", file.path, "\n");
    absl::StrAppend(&diff, "@@ -0,0 +1,", lines.size(), " @@\n");
    for (const std::string_view line : lines) {
      absl::StrAppend(&diff, "+", line, "\n");
    }
    if (!ends_with_newline && !lines.empty()) {
      // git records this explicitly, and without it the applied file gains a
      // newline the submitter did not write.
      absl::StrAppend(&diff, "\\ No newline at end of file\n");
    }
  }
  return diff;
}

bool PathMatchesGlob(std::string_view path, std::string_view pattern) {
  // Recursive descent over the two strings. Patterns are short and come from a
  // config file, so the simple form is the right one.
  if (pattern.empty()) {
    return path.empty();
  }
  if (pattern.starts_with("**")) {
    std::string_view rest = pattern.substr(2);
    // "**/" also matches zero directories, so "a/**/b" matches "a/b".
    if (rest.starts_with("/") && PathMatchesGlob(path, rest.substr(1))) {
      return true;
    }
    for (std::size_t skip = 0; skip <= path.size(); ++skip) {
      if (PathMatchesGlob(path.substr(skip), rest)) {
        return true;
      }
    }
    return false;
  }
  if (pattern.front() == '*') {
    const std::string_view rest = pattern.substr(1);
    for (std::size_t skip = 0; skip <= path.size(); ++skip) {
      // A single '*' stops at a separator.
      if (skip > 0 && path[skip - 1] == '/') {
        break;
      }
      if (PathMatchesGlob(path.substr(skip), rest)) {
        return true;
      }
    }
    return false;
  }
  if (path.empty() || path.front() != pattern.front()) {
    return false;
  }
  return PathMatchesGlob(path.substr(1), pattern.substr(1));
}

}  // namespace tournament_arena
