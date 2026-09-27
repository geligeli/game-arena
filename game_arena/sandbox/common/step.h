#ifndef GAME_ARENA_GAME_ARENA_SANDBOX_COMMON_STEP_H
#define GAME_ARENA_GAME_ARENA_SANDBOX_COMMON_STEP_H

#include <chrono>
#include <filesystem>
#include <string>
#include <vector>

#include "game_arena/common/process/process.h"

namespace sandbox_common {

struct StepResult {
  process::RunResult run;
  std::string output;  // stdout then stderr
};

// Output goes to <log_dir>/<tag>.{out,err}.
StepResult RunStep(const std::string &executable,
                   const std::vector<std::string> &args,
                   const std::filesystem::path &cwd,
                   const std::filesystem::path &log_dir, const std::string &tag,
                   std::chrono::seconds timeout,
                   const std::filesystem::path &stdin_path = {});

}  // namespace sandbox_common

#endif  // GAME_ARENA_GAME_ARENA_SANDBOX_COMMON_STEP_H
