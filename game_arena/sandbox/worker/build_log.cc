#include "game_arena/sandbox/worker/build_log.h"

#include <cstdio>
#include <sstream>
#include <vector>

#include "game_arena/sandbox/common/text.h"
#include "re2/re2.h"

namespace tournament_arena {

namespace {

bool IsInteresting(const std::string &line) {
  static const RE2 *const kPatterns[] = {
      // file.cc:12:34: error: ...
      new RE2(R"(^\s*\S+\.(cc|cpp|h|hpp|inl):\d+(:\d+)?:)"),
      new RE2(R"(\b(error|fatal error|undefined reference):)"),
      new RE2(R"(^ERROR:)"),
      // "In file included from" and its "from x.h:3," follow-ons.
      new RE2(R"(^\s*(In file included from |from )\S)"),
      // The chain that explains a template error.
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

std::string CompactBuildLog(const std::string &log, BuildLogLimits limits) {
  std::vector<std::string> kept;
  std::istringstream lines(log);
  std::string line;
  while (std::getline(lines, line)) {
    if (IsInteresting(line)) {
      kept.push_back(line);
    }
  }

  if (kept.empty()) {
    return TailOf(log, limits.max_chars);
  }

  const bool trimmed = static_cast<int>(kept.size()) > limits.max_lines;
  if (trimmed) {
    // The first errors are the real ones; the rest tend to be fallout.
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
