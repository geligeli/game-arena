#ifndef GAME_ARENA_GAME_ARENA_COMMON_PROCESS_PROCESS_H
#define GAME_ARENA_GAME_ARENA_COMMON_PROCESS_PROCESS_H
#include <sys/types.h>

#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <ostream>
#include <string>
#include <vector>

namespace process {

struct InputStreamProcess {
  InputStreamProcess();
  InputStreamProcess(InputStreamProcess&& other) noexcept;
  InputStreamProcess& operator=(InputStreamProcess&& other) noexcept;
  ~InputStreamProcess();

  InputStreamProcess(const InputStreamProcess&) = delete;
  auto operator=(const InputStreamProcess&) -> InputStreamProcess& = delete;

  std::ostream& stdin();
  int Wait();

 private:
  struct Impl;
  explicit InputStreamProcess(std::unique_ptr<Impl> impl);

  std::unique_ptr<Impl> impl_;

  friend InputStreamProcess CreateInputStreamProcess(
      const std::string& executable, const std::vector<std::string>& arguments,
      const std::vector<std::string>& env,
      std::filesystem::path const& stdout_path,
      std::filesystem::path const& stderr_path);
};

InputStreamProcess CreateInputStreamProcess(
    const std::string& executable, const std::vector<std::string>& arguments,
    const std::vector<std::string>& env,
    std::filesystem::path const& stdout_path = "/dev/null",
    std::filesystem::path const& stderr_path = "/dev/null");

// ---------------------------------------------------------------------------
// Run to completion
// ---------------------------------------------------------------------------

struct RunOptions {
  std::filesystem::path cwd;          // empty: inherit the caller's
  std::vector<std::string> env;       // empty: inherit the caller's
  std::filesystem::path stdout_path;  // empty: /dev/null
  std::filesystem::path stderr_path;  // empty: /dev/null
  // Wall-clock limit. Zero waits indefinitely. On expiry the child's whole
  // process group is signalled, not just the child: build tools spawn trees,
  // and killing only the parent leaves the workers running.
  std::chrono::seconds timeout{0};
  // Grace between SIGTERM and SIGKILL when a timeout fires.
  std::chrono::seconds kill_grace{5};
  // RLIMIT_AS for the child, in bytes. Zero leaves it unlimited. A cap makes
  // an over-allocating child fail its own allocation rather than push the host
  // into swap or the OOM killer.
  std::size_t address_space_limit_bytes = 0;

  // Called in the parent with the child's pgid, once, right after the child is
  // in its own process group. Lets a caller abort a run it is not the one
  // waiting on -- `killpg(pgid, SIGKILL)` reaches the whole tree, which is what
  // a build tool needs. Runs on the calling thread before the wait begins, so
  // it must not block.
  std::function<void(pid_t)> on_started;
};

struct RunResult {
  int exit_code = -1;  // 128 + signal when killed by one
  bool timed_out = false;
  bool started = false;  // false when the executable could not be launched
};

// Runs |executable| to completion with its own process group. |executable| is
// resolved through PATH when it contains no '/'.
auto RunCommand(const std::string& executable,
                const std::vector<std::string>& arguments,
                const RunOptions& options) -> RunResult;

// Finds |name| on PATH, or returns it unchanged when it already contains '/'.
// Empty when nothing executable matches.
auto ResolveExecutable(const std::string& name) -> std::string;

}  // namespace process

#endif  // GAME_ARENA_GAME_ARENA_COMMON_PROCESS_PROCESS_H