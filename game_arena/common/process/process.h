#ifndef GAME_ARENA_GAME_ARENA_COMMON_PROCESS_PROCESS_H
#define GAME_ARENA_GAME_ARENA_COMMON_PROCESS_PROCESS_H
#include <sys/types.h>

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace process {

// Empty paths inherit. Every field has an initializer, for designated init.
struct Options {
  std::filesystem::path cwd = {};
  // "K=V" entries laid over the caller's environment.
  std::vector<std::string> env = {};
  std::filesystem::path stdin_path = {};
  std::filesystem::path stdout_path = {};
  std::filesystem::path stderr_path = {};
  // RLIMIT_AS; 0 is unlimited. An over-allocating child fails, not the host.
  std::size_t address_space_limit_bytes = 0;
};

// A child in its own process group, so stopping it stops what it spawned.
// Destroying a running Child stops it.
class Child {
 public:
  // Nullopt when |executable| cannot be launched. Resolved through PATH when
  // it contains no '/'; a relative path with one is taken relative to
  // |options.cwd| when that is set.
  static std::optional<Child> Start(const std::string& executable,
                                    const std::vector<std::string>& arguments,
                                    const Options& options);

  Child(Child&& other) noexcept;
  ~Child();
  Child(const Child&) = delete;
  Child& operator=(const Child&) = delete;

  pid_t pid() const { return pid_; }

  // The exit code (128 + signal) once exited, nullopt while running.
  std::optional<int> Poll();

  // Blocks until the child exits and returns its exit code, or nullopt once a
  // nonzero |timeout| expires with the child still running.
  std::optional<int> Wait(std::chrono::seconds timeout = {});

  // SIGTERM the group, wait up to |grace|, then SIGKILL it. Returns the exit
  // code, or the recorded one if it already exited.
  int Stop(std::chrono::seconds grace);

 private:
  explicit Child(pid_t pid) : pid_(pid) {}

  pid_t pid_ = -1;
  std::optional<int> exit_code_;
};

struct RunResult {
  int exit_code = -1;  // 128 + signal when killed by one
  bool timed_out = false;
  bool started = false;  // false when the executable could not be launched
};

// Runs |executable| to completion. A nonzero |timeout| stops it on expiry.
RunResult RunCommand(const std::string& executable,
                     const std::vector<std::string>& arguments,
                     const Options& options, std::chrono::seconds timeout = {});

// Finds |name| on PATH, or returns it unchanged when it already contains '/'.
// Empty when nothing executable matches.
std::string ResolveExecutable(const std::string& name);

}  // namespace process

#endif  // GAME_ARENA_GAME_ARENA_COMMON_PROCESS_PROCESS_H
