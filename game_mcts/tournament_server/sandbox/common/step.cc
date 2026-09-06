#include "game_mcts/tournament_server/sandbox/common/step.h"

#include "game_mcts/tournament_server/sandbox/common/files.h"

namespace sandbox_common {

auto RunStep(const std::string &executable,
             const std::vector<std::string> &args,
             const std::filesystem::path &cwd,
             const std::filesystem::path &log_dir, const std::string &tag,
             std::chrono::seconds timeout,
             std::size_t address_space_limit_bytes,
             const std::function<void(pid_t)> &on_started) -> StepResult {
  process::RunOptions options;
  options.on_started = on_started;
  options.cwd = cwd;
  options.stdout_path = log_dir / (tag + ".out");
  options.stderr_path = log_dir / (tag + ".err");
  options.timeout = timeout;
  options.address_space_limit_bytes = address_space_limit_bytes;

  StepResult result;
  result.run = process::RunCommand(executable, args, options);
  result.output = ReadFile(options.stdout_path) + ReadFile(options.stderr_path);
  return result;
}

}  // namespace sandbox_common
