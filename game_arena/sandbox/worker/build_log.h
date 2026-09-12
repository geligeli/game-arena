#ifndef GAME_ARENA_GAME_ARENA_SANDBOX_WORKER_BUILD_LOG_H
#define GAME_ARENA_GAME_ARENA_SANDBOX_WORKER_BUILD_LOG_H

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

}  // namespace tournament_arena

#endif  // GAME_ARENA_GAME_ARENA_SANDBOX_WORKER_BUILD_LOG_H
