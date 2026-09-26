#include "game_arena/sandbox/common/step.h"

#include "game_arena/sandbox/common/files.h"

namespace sandbox_common {

StepResult RunStep(const std::string &executable,
                   const std::vector<std::string> &args,
                   const std::filesystem::path &cwd,
                   const std::filesystem::path &log_dir, const std::string &tag,
                   std::chrono::seconds timeout,
                   const std::filesystem::path &stdin_path) {
  const process::Options options = {.cwd = cwd,
                                    .stdin_path = stdin_path,
                                    .stdout_path = log_dir / (tag + ".out"),
                                    .stderr_path = log_dir / (tag + ".err")};

  StepResult result;
  result.run = process::RunCommand(executable, args, options, timeout);
  result.output = ReadFile(options.stdout_path) + ReadFile(options.stderr_path);
  return result;
}

}  // namespace sandbox_common
