#ifndef RISK_GAME_AI_CPP_TOURNAMENT_SERVER_SANDBOX_WORKER_BUILD_LOG_H
#define RISK_GAME_AI_CPP_TOURNAMENT_SERVER_SANDBOX_WORKER_BUILD_LOG_H

// Turns a bazel build log into something worth sending to an agent.
//
// This is the system's biggest token lever. A failing build is megabytes of
// action graph chatter; what the agent needs is the twenty lines carrying
// file:line and "error:". Compacting here -- on the worker, before the bytes
// cross the wire -- means the arena never stores, forwards or re-serves the
// full log.
//
// The same idea as _extract_gtest_failures in mcp_servers/bazel_mcp/server.py,
// moved to where the build actually happens.

#include <cstddef>
#include <string>

namespace tournament_arena {

struct BuildLogLimits {
  std::size_t max_chars = 6000;
  int max_lines = 60;
};

// Pulls the diagnostic lines out of |log|: compiler errors with file:line,
// bazel's own ERROR lines, and the "In file included from" chains that say
// which candidate header pulled in the failure. Falls back to the tail of the
// log when nothing matches, since an unrecognised failure still has to be
// diagnosable.
auto CompactBuildLog(const std::string &log,
                     BuildLogLimits limits = {}) -> std::string;

// Keeps the last |max_chars| of |text|, marking what was dropped. Errors are
// at the end of a build log.
auto TailOf(const std::string &text, std::size_t max_chars) -> std::string;

// Parses the "RESULT games=N wins=W draws=D losses=L elo=E" line the candidate
// harness prints. Returns false when no such line is present.
struct RunTally {
  int games = 0;
  int wins = 0;
  int draws = 0;
  int losses = 0;
  double elo = 0.0;
};
auto ParseResultLine(const std::string &output, RunTally *tally) -> bool;

}  // namespace tournament_arena

#endif  // RISK_GAME_AI_CPP_TOURNAMENT_SERVER_SANDBOX_WORKER_BUILD_LOG_H
