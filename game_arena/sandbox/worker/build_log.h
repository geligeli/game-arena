#ifndef GAME_ARENA_GAME_ARENA_SANDBOX_WORKER_BUILD_LOG_H
#define GAME_ARENA_GAME_ARENA_SANDBOX_WORKER_BUILD_LOG_H

// Compacted on the worker, so a full bazel log never crosses the wire.

#include <cstddef>
#include <string>

namespace tournament_arena {

struct BuildLogLimits {
  std::size_t max_chars = 6000;
  int max_lines = 60;
};

// The diagnostic lines of |log|, or its tail when none match.
std::string CompactBuildLog(const std::string &log, BuildLogLimits limits = {});

}  // namespace tournament_arena

#endif  // GAME_ARENA_GAME_ARENA_SANDBOX_WORKER_BUILD_LOG_H
