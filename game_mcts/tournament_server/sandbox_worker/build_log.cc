#include "cpp/tournament_server/sandbox_worker/build_log.h"

#include <cstdio>
#include <sstream>
#include <vector>

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

}  // namespace

auto TailOf(const std::string &text, std::size_t max_chars) -> std::string {
  if (text.size() <= max_chars) {
    return text;
  }
  return "... (" + std::to_string(text.size() - max_chars) +
         " chars truncated) ...\n" + text.substr(text.size() - max_chars);
}

auto CompactBuildLog(const std::string &log, BuildLogLimits limits)
    -> std::string {
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

auto ParseResultLine(const std::string &output, RunTally *tally) -> bool {
  static const RE2 kResult(
      R"(RESULT games=(\d+) wins=(\d+) draws=(\d+) losses=(\d+) elo=([-\d.]+))");
  std::istringstream lines(output);
  std::string line;
  bool found = false;
  while (std::getline(lines, line)) {
    RunTally parsed;
    if (RE2::PartialMatch(line, kResult, &parsed.games, &parsed.wins,
                          &parsed.draws, &parsed.losses, &parsed.elo)) {
      // Last one wins: a retried run appends rather than replaces.
      *tally = parsed;
      found = true;
    }
  }
  return found;
}

}  // namespace tournament_arena
