#include "game_arena/sandbox/worker/build_log.h"

#include <cstdio>
#include <sstream>
#include <vector>

#include "game_arena/sandbox/common/text.h"
#include "re2/re2.h"

namespace tournament_arena {

namespace {

// A line is worth keeping if it says what broke or where. Ordered roughly by
// how often each one carries the answer.
auto IsInteresting(const std::string &line) -> bool {
  static const RE2 *const kPatterns[] = {
      // file.cc:12:34: error: ...  (gcc/clang, the usual answer)
      new RE2(R"(^\s*\S+\.(cc|cpp|h|hpp|inl):\d+(:\d+)?:)"),
      new RE2(R"(\b(error|fatal error|undefined reference):)"),
      // bazel's own failures: unknown target, bad label, missing dep.
      new RE2(R"(^ERROR:)"),
      // The include chain, and its "                 from x.h:3," follow-ons,
      // which say which of the candidate's headers pulled the failure in.
      new RE2(R"(^\s*(In file included from |from )\S)"),
      // The "required from here" chain that explains a template error.
      new RE2(R"(required from|instantiation of|in expansion of macro)"),
      new RE2(R"(^Use --sandbox_debug|^\s*\^)"),
  };
  for (const RE2 *pattern : kPatterns) {
    if (RE2::PartialMatch(line, *pattern)) {
      return true;
    }
  }
  return false;
}

using sandbox_common::TailOf;

}  // namespace

auto CompactBuildLog(const std::string &log,
                     BuildLogLimits limits) -> std::string {
  std::vector<std::string> kept;
  std::istringstream lines(log);
  std::string line;
  while (std::getline(lines, line)) {
    if (IsInteresting(line)) {
      kept.push_back(line);
    }
  }

  if (kept.empty()) {
    // An unrecognised failure still has to be diagnosable, so fall back to the
    // end of the log rather than reporting nothing.
    return TailOf(log, limits.max_chars);
  }

  const bool trimmed = static_cast<int>(kept.size()) > limits.max_lines;
  if (trimmed) {
    // The first errors are the real ones; everything after tends to be
    // fallout from them.
    kept.resize(limits.max_lines);
  }

  std::string out;
  for (const std::string &kept_line : kept) {
    out += kept_line;
    out += '\n';
  }
  if (trimmed) {
    out += "... (further diagnostics omitted) ...\n";
  }
  return TailOf(out, limits.max_chars);
}

}  // namespace tournament_arena
