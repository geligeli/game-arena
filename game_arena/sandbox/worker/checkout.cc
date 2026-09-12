#include "game_arena/sandbox/worker/checkout.h"

#include <chrono>
#include <system_error>

#include "game_arena/sandbox/common/step.h"
#include "game_arena/sandbox/common/text.h"

namespace tournament_arena {

using sandbox_common::TailOf;

auto EnsureClone(const std::string &git, const std::string &source,
                 const std::filesystem::path &dest,
                 const std::filesystem::path &log_dir,
                 std::string *error) -> bool {
  std::error_code ec;
  std::filesystem::create_directories(log_dir, ec);
  if (std::filesystem::exists(dest / ".git")) {
    return true;
  }
  std::filesystem::create_directories(dest.parent_path(), ec);
  const sandbox_common::StepResult clone = sandbox_common::RunStep(
      git, {"clone", "--local", source, dest.string()}, dest.parent_path(),
      log_dir, "clone", std::chrono::seconds(900));
  if (!clone.run.started || clone.run.exit_code != 0) {
    *error = "git clone failed: " + TailOf(clone.output, 2000);
    return false;
  }
  return true;
}

auto SyncToCommit(const std::string &git, const std::filesystem::path &repo,
                  const std::string &commit,
                  const std::filesystem::path &log_dir,
                  std::string *error) -> bool {
  if (commit.empty()) {
    return true;
  }

  const sandbox_common::StepResult fetch = sandbox_common::RunStep(
      git, {"fetch", "--all", "--tags", "--quiet"}, repo, log_dir, "fetch",
      std::chrono::seconds(600));
  (void)fetch;  // A stale mirror is survivable; the checkout below decides.

  const sandbox_common::StepResult checkout =
      sandbox_common::RunStep(git, {"checkout", "--force", commit}, repo,
                              log_dir, "checkout", std::chrono::seconds(300));
  if (!checkout.run.started || checkout.run.exit_code != 0) {
    *error =
        "git checkout " + commit + " failed: " + TailOf(checkout.output, 2000);
    return false;
  }
  return true;
}

}  // namespace tournament_arena
