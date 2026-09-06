#ifndef GAME_ARENA_GAME_ARENA_SANDBOX_COMMON_STEP_H
#define GAME_ARENA_GAME_ARENA_SANDBOX_COMMON_STEP_H

// One subprocess step of an order, with its output captured to files.
//
// Shared by the sandbox backends so a step is launched, logged and timed out
// the same way whether it runs on the host or starts a container. stdout and
// stderr stay apart on disk -- combining them into one file would interleave
// unpredictably -- and are concatenated only when reporting.

#include <sys/types.h>

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "game_arena/common/process/process.h"

namespace sandbox_common {

struct StepResult {
  process::RunResult run;
  std::string output;  // stdout followed by stderr
};

// Runs |executable| with output captured to <log_dir>/<tag>.{out,err} and a
// wall-clock timeout. |on_started| publishes the child's process group, so a
// caller can abort a step it is not the one waiting on.
auto RunStep(const std::string &executable,
             const std::vector<std::string> &args,
             const std::filesystem::path &cwd,
             const std::filesystem::path &log_dir, const std::string &tag,
             std::chrono::seconds timeout,
             std::size_t address_space_limit_bytes = 0,
             const std::function<void(pid_t)> &on_started = {}) -> StepResult;

}  // namespace sandbox_common

#endif  // GAME_ARENA_GAME_ARENA_SANDBOX_COMMON_STEP_H
