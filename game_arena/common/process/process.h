#ifndef GAME_ARENA_GAME_ARENA_COMMON_PROCESS_PROCESS_H
#define GAME_ARENA_GAME_ARENA_COMMON_PROCESS_PROCESS_H
#include <sys/types.h>

#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
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
// resolved through PATH when it contains no '/'; a relative path with one is
// taken relative to |options.cwd| when that is set.
auto RunCommand(const std::string& executable,
                const std::vector<std::string>& arguments,
                const RunOptions& options) -> RunResult;

// ---------------------------------------------------------------------------
// Long-running children
// ---------------------------------------------------------------------------

struct ChildOptions {
  std::filesystem::path cwd;  // empty: inherit the caller's
  // "K=V" entries added to the caller's environment, overriding any the caller
  // already has. Unlike RunOptions::env this never replaces the environment
  // wholesale: a supervised server should see the same PATH and HOME as the
  // supervisor, plus what it is told.
  std::vector<std::string> extra_env;
  std::filesystem::path stdout_path;  // empty: inherit the caller's
  std::filesystem::path stderr_path;  // empty: inherit the caller's
};

// A child that is meant to keep running -- a server this process supervises.
// Its own process group, like RunCommand, so stopping it stops what it
// spawned. Destroying a running Child kills it: a supervisor that exits
// leaves nothing behind.
class Child {
 public:
  // Nullopt when |executable| cannot be launched. Resolved through PATH when
  // it contains no '/'.
  static auto Start(const std::string& executable,
                    const std::vector<std::string>& arguments,
                    const ChildOptions& options) -> std::optional<Child>;

  Child(Child&& other) noexcept;
  auto operator=(Child&& other) noexcept -> Child&;
  ~Child();
  Child(const Child&) = delete;
  auto operator=(const Child&) -> Child& = delete;

  auto pid() const -> pid_t { return pid_; }

  // The exit code once the child has exited (128 + signal when killed by
  // one), nullopt while it runs. Reaps the child; later calls return the same
  // code.
  auto Poll() -> std::optional<int>;

  // Sends |signum| to the child's whole process group.
  void Signal(int signum) const;

  // Blocks until the child exits and returns its exit code.
  auto Wait() -> int;

  // SIGTERM the group, wait up to |grace|, then SIGKILL it. Returns the exit
  // code. A no-op returning the recorded code if it already exited.
  auto Stop(std::chrono::seconds grace) -> int;

 private:
  explicit Child(pid_t pid) : pid_(pid) {}

  pid_t pid_ = -1;
  std::optional<int> exit_code_;
};

// Finds |name| on PATH, or returns it unchanged when it already contains '/'.
// Empty when nothing executable matches.
auto ResolveExecutable(const std::string& name) -> std::string;

}  // namespace process

#endif  // GAME_ARENA_GAME_ARENA_COMMON_PROCESS_PROCESS_H