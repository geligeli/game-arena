#include "game_arena/common/process/process.h"

#include <fcntl.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstdlib>

extern char** environ;

namespace process {

namespace {

// Points |fd| at |path|; an empty path leaves it as inherited.
bool Redirect(const std::filesystem::path& path, int fd, int flags) {
  if (path.empty()) {
    return true;
  }
  const int opened = ::open(path.c_str(), flags, 0644);
  if (opened == -1 || ::dup2(opened, fd) == -1) {
    return false;
  }
  if (opened != fd) {
    ::close(opened);
  }
  return true;
}

// The caller's environment with |extra| laid over it, "K=V" by key.
std::vector<std::string> MergedEnvironment(
    const std::vector<std::string>& extra) {
  std::vector<std::string> merged;
  for (char** entry = environ; entry != nullptr && *entry != nullptr; ++entry) {
    merged.emplace_back(*entry);
  }
  for (const std::string& kv : extra) {
    const std::string key = kv.substr(0, kv.find('='));
    std::erase_if(merged, [&](const std::string& have) {
      return have.compare(0, key.size() + 1, key + "=") == 0;
    });
    merged.push_back(kv);
  }
  return merged;
}

std::vector<char*> Pointers(std::vector<std::string>& strings) {
  std::vector<char*> pointers;
  for (std::string& s : strings) {
    pointers.push_back(s.data());
  }
  pointers.push_back(nullptr);
  return pointers;
}

}  // namespace

std::string ResolveExecutable(const std::string& name) {
  if (name.empty()) {
    return {};
  }
  if (name.find('/') != std::string::npos) {
    return ::access(name.c_str(), X_OK) == 0 ? name : std::string{};
  }
  const char* path_env = ::getenv("PATH");
  if (path_env == nullptr) {
    return {};
  }
  const std::string path(path_env);
  size_t begin = 0;
  while (begin <= path.size()) {
    const size_t colon = path.find(':', begin);
    const std::string dir = path.substr(
        begin, colon == std::string::npos ? std::string::npos : colon - begin);
    if (!dir.empty()) {
      const std::string candidate = dir + "/" + name;
      if (::access(candidate.c_str(), X_OK) == 0) {
        return candidate;
      }
    }
    if (colon == std::string::npos) {
      break;
    }
    begin = colon + 1;
  }
  return {};
}

std::optional<Child> Child::Start(const std::string& executable,
                                  const std::vector<std::string>& arguments,
                                  const Options& options) {
  // "bazel-bin/grader/grade" means the one in |cwd|, not the caller's.
  const bool in_cwd = !options.cwd.empty() && !executable.starts_with('/') &&
                      executable.find('/') != std::string::npos;
  std::vector<std::string> args = {ResolveExecutable(
      in_cwd ? (options.cwd / executable).string() : executable)};
  if (args[0].empty()) {
    return std::nullopt;
  }
  args.insert(args.end(), arguments.begin(), arguments.end());
  std::vector<std::string> env = MergedEnvironment(options.env);
  const std::vector<char*> argv = Pointers(args);
  const std::vector<char*> envp = Pointers(env);
  // Soft and hard together, so the child cannot raise it back.
  const rlim_t bytes = options.address_space_limit_bytes;
  const struct rlimit limit = {bytes, bytes};

  const pid_t pid = ::fork();
  if (pid == -1) {
    return std::nullopt;
  }
  if (pid == 0) {
    ::setpgid(0, 0);
    constexpr int kWrite = O_CREAT | O_WRONLY | O_TRUNC;
    if ((!options.cwd.empty() && ::chdir(options.cwd.c_str()) == -1) ||
        !Redirect(options.stdin_path, STDIN_FILENO, O_RDONLY) ||
        !Redirect(options.stdout_path, STDOUT_FILENO, kWrite) ||
        !Redirect(options.stderr_path, STDERR_FILENO, kWrite) ||
        (bytes > 0 && ::setrlimit(RLIMIT_AS, &limit) == -1)) {
      _exit(127);
    }
    ::execve(argv[0], argv.data(), envp.data());
    _exit(127);
  }
  // Also set from the parent: whichever runs first wins, and neither side may
  // assume the other has been scheduled yet.
  ::setpgid(pid, pid);
  return Child(pid);
}

Child::Child(Child&& other) noexcept
    : pid_(other.pid_), exit_code_(other.exit_code_) {
  other.pid_ = -1;
}

Child& Child::operator=(Child&& other) noexcept {
  if (this != &other) {
    if (pid_ > 0) {
      Stop(std::chrono::seconds(2));
    }
    pid_ = other.pid_;
    exit_code_ = other.exit_code_;
    other.pid_ = -1;
  }
  return *this;
}

Child::~Child() {
  if (pid_ > 0) {
    Stop(std::chrono::seconds(2));
  }
}

std::optional<int> Child::Poll() {
  if (exit_code_ || pid_ <= 0) {
    return exit_code_;
  }
  int status = 0;
  pid_t waited;
  while ((waited = ::waitpid(pid_, &status, WNOHANG)) == -1 && errno == EINTR) {
  }
  if (waited != pid_) {
    return std::nullopt;
  }
  exit_code_ = WIFEXITED(status)     ? WEXITSTATUS(status)
               : WIFSIGNALED(status) ? 128 + WTERMSIG(status)
                                     : -1;
  // Reap anything else the group left behind.
  ::kill(-pid_, SIGKILL);
  while (::waitpid(-pid_, nullptr, WNOHANG) > 0) {
  }
  return exit_code_;
}

int Child::Wait() {
  while (!Poll()) {
    ::usleep(20000);
  }
  return *exit_code_;
}

int Child::Stop(std::chrono::seconds grace) {
  if (Poll()) {
    return *exit_code_;
  }
  ::kill(-pid_, SIGTERM);
  const auto deadline = std::chrono::steady_clock::now() + grace;
  while (!Poll()) {
    if (std::chrono::steady_clock::now() >= deadline) {
      ::kill(-pid_, SIGKILL);
    }
    ::usleep(20000);
  }
  return *exit_code_;
}

RunResult RunCommand(const std::string& executable,
                     const std::vector<std::string>& arguments,
                     const Options& options, std::chrono::seconds timeout,
                     const std::function<void(pid_t)>& on_started) {
  RunResult result;
  std::optional<Child> child = Child::Start(executable, arguments, options);
  if (!child) {
    return result;
  }
  result.started = true;
  if (on_started) {
    on_started(child->pid());
  }
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (!child->Poll()) {
    if (timeout.count() > 0 && std::chrono::steady_clock::now() >= deadline) {
      result.timed_out = true;
      break;
    }
    ::usleep(20000);
  }
  result.exit_code = child->Stop(std::chrono::seconds(5));
  return result;
}

}  // namespace process
