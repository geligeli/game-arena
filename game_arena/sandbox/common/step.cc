#include "game_arena/sandbox/common/step.h"

#include "game_arena/sandbox/common/files.h"

namespace sandbox_common {

StepResult RunStep(const std::string &executable,
                   const std::vector<std::string> &args,
                   const std::filesystem::path &cwd,
                   const std::filesystem::path &log_dir, const std::string &tag,
                   std::chrono::seconds timeout,
                   std::size_t address_space_limit_bytes,
                   const std::function<void(pid_t)> &on_started,
                   const std::vector<std::string> &env,
                   const std::filesystem::path &stdin_path) {
  const process::Options options = {
      .cwd = cwd,
      .env = env,
      .stdin_path = stdin_path,
      .stdout_path = log_dir / (tag + ".out"),
      .stderr_path = log_dir / (tag + ".err"),
      .address_space_limit_bytes = address_space_limit_bytes};

  StepResult result;
  result.run =
      process::RunCommand(executable, args, options, timeout, on_started);
  result.output = ReadFile(options.stdout_path) + ReadFile(options.stderr_path);
  return result;
}

}  // namespace sandbox_common
