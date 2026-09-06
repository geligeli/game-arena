#include "game_mcts/tournament_server/server/unified_diff.h"

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "absl/strings/str_cat.h"

namespace tournament_arena {

namespace {

auto StartsWith(std::string_view text, std::string_view prefix) -> bool {
  return text.size() >= prefix.size() &&
         text.substr(0, prefix.size()) == prefix;
}

// Splits on '\n', keeping empty lines. A trailing newline does not produce a
// final empty line, so a diff ending in "\n" has no phantom entry.
auto SplitLines(std::string_view text) -> std::vector<std::string_view> {
  std::vector<std::string_view> lines;
  std::size_t at = 0;
  while (at <= text.size()) {
    const std::size_t nl = text.find('\n', at);
    if (nl == std::string_view::npos) {
      if (at < text.size()) {
        lines.push_back(text.substr(at));
      }
      break;
    }
    lines.push_back(text.substr(at, nl - at));
    at = nl + 1;
  }
  return lines;
}

// Strips the "a/" or "b/" git prepends, and trims the trailing tab-timestamp a
// plain `diff -u` leaves behind.
auto CleanPath(std::string_view raw) -> std::string {
  const std::size_t tab = raw.find('\t');
  if (tab != std::string_view::npos) {
    raw = raw.substr(0, tab);
  }
  if (raw == "/dev/null") {
    return {};
  }
  if (StartsWith(raw, "a/") || StartsWith(raw, "b/")) {
    raw = raw.substr(2);
  }
  return std::string(raw);
}

// The same rules the store applies to any path it writes under: relative, no
// '..', no absolute escape. Checked here because a patch header is the one
// place a path arrives as free text.
auto IsUsablePath(const std::string &path, std::string *error) -> bool {
  if (path.empty()) {
    *error = "a patch entry has an empty path";
    return false;
  }
  if (path.front() == '/') {
    *error =
        absl::StrCat("path '", path, "' must be relative to the repo root");
    return false;
  }
  std::size_t at = 0;
  while (at <= path.size()) {
    const std::size_t slash = path.find('/', at);
    const std::string_view part = std::string_view(path).substr(
        at, slash == std::string::npos ? std::string::npos : slash - at);
    if (part == ".." || part == ".") {
      *error =
          absl::StrCat("path '", path, "' contains a '", part, "' component");
      return false;
    }
    if (slash == std::string::npos) {
      break;
    }
    at = slash + 1;
  }
  return true;
}

}  // namespace

auto ParseUnifiedDiff(std::string_view diff, Patch *out,
                      std::string *error) -> bool {
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
    if (StartsWith(line, "diff --git ")) {
      flush();
      in_file = true;
      continue;
    }
    if (StartsWith(line, "new file mode")) {
      current.is_new = true;
      continue;
    }
    if (StartsWith(line, "deleted file mode")) {
      current.is_delete = true;
      continue;
    }
    if (StartsWith(line, "--- ")) {
      // A bare `diff -u` has no "diff --git" line, so the ---/+++ pair is what
      // starts a file. Only treat it as a new entry when one is not open.
      if (!in_file) {
        in_file = true;
      }
      current.old_path = CleanPath(line.substr(4));
      if (current.old_path.empty()) {
        current.is_new = true;
      }
      continue;
    }
    if (StartsWith(line, "+++ ")) {
      current.new_path = CleanPath(line.substr(4));
      if (current.new_path.empty()) {
        current.is_delete = true;
      }
      continue;
    }
    if (StartsWith(line, "@@")) {
      if (!in_file) {
        *error = "a hunk appears before any file header";
        return false;
      }
      ++current.hunks;
      ++out->total_hunks;
      collecting = current.is_new;
      continue;
    }
    if (collecting && StartsWith(line, "+")) {
      current.added_content.append(line.substr(1));
      current.added_content.push_back('\n');
      continue;
    }
    if (collecting && StartsWith(line, "\\ No newline at end of file")) {
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

auto TouchedPaths(const Patch &patch) -> std::vector<std::string> {
  std::vector<std::string> paths;
  for (const PatchFile &file : patch.files) {
    for (const std::string &path : {file.old_path, file.new_path}) {
      if (!path.empty() &&
          std::find(paths.begin(), paths.end(), path) == paths.end()) {
        paths.push_back(path);
      }
    }
  }
  return paths;
}

auto MakeAddOnlyPatch(const std::vector<NewFile> &files) -> std::string {
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

auto PathMatchesGlob(std::string_view path, std::string_view pattern) -> bool {
  // Recursive descent over the two strings. Patterns are short and come from a
  // config file, so the simple form is the right one.
  if (pattern.empty()) {
    return path.empty();
  }
  if (StartsWith(pattern, "**")) {
    std::string_view rest = pattern.substr(2);
    // "**/" also matches zero directories, so "a/**/b" matches "a/b".
    if (StartsWith(rest, "/") && PathMatchesGlob(path, rest.substr(1))) {
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
