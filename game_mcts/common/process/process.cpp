#include "game_mcts/common/process/process.h"

#include <fcntl.h>
#include <sys/resource.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <streambuf>
#include <system_error>
#include <utility>
#include <vector>

extern char** environ;

namespace process {

namespace {

class FdOutputBuffer : public std::streambuf {
 public:
  explicit FdOutputBuffer(int fd) : fd_(fd) {}
  ~FdOutputBuffer() override { Close(); }

  void Close() {
    if (fd_ < 0) {
      return;
    }
    while (::close(fd_) == -1 && errno == EINTR) {
    }
    fd_ = -1;
  }

 protected:
  auto overflow(int_type ch) -> int_type override {
    if (fd_ < 0) {
      throw std::runtime_error("stdin pipe already closed");
    }
    if (ch == traits_type::eof()) {
      return traits_type::not_eof(ch);
    }
    char c = static_cast<char>(ch);
    WriteAll(&c, 1);
    return ch;
  }

  auto xsputn(const char* s,
              std::streamsize count) -> std::streamsize override {
    if (fd_ < 0) {
      throw std::runtime_error("stdin pipe already closed");
    }
    WriteAll(s, count);
    return count;
  }

  auto sync() -> int override { return 0; }

 private:
  void WriteAll(const char* data, std::streamsize len) {
    const char* ptr = data;
    std::streamsize remaining = len;
    while (remaining > 0) {
      ssize_t written = ::write(fd_, ptr, static_cast<size_t>(remaining));
      if (written == -1) {
        if (errno == EINTR) {
          continue;
        }
        throw std::system_error(errno, std::generic_category(), "write");
      }
      remaining -= written;
      ptr += written;
    }
  }

  int fd_;
};

struct ExecVectors {
  std::vector<std::string> argv_storage;
  std::vector<char*> argv;
  std::vector<std::string> env_storage;
  std::vector<char*> envp;
};

auto BuildExecVectors(const std::string& executable,
                      const std::vector<std::string>& arguments,
                      const std::vector<std::string>& env) -> ExecVectors {
  ExecVectors vectors;
  vectors.argv_storage.reserve(arguments.size() + 1);
  vectors.argv_storage.push_back(executable);
  for (const auto& arg : arguments) {
    vectors.argv_storage.push_back(arg);
  }
  vectors.argv.reserve(vectors.argv_storage.size() + 1);
  for (auto& entry : vectors.argv_storage) {
    vectors.argv.push_back(entry.data());
  }
  vectors.argv.push_back(nullptr);

  vectors.env_storage = env;
  if (!vectors.env_storage.empty()) {
    vectors.envp.reserve(vectors.env_storage.size() + 1);
    for (auto& entry : vectors.env_storage) {
      vectors.envp.push_back(entry.data());
    }
    vectors.envp.push_back(nullptr);
  }

  return vectors;
}

auto RedirectStream(const std::filesystem::path& path, int target_fd) -> bool {
  if (path.empty()) {
    return true;
  }
  int fd = ::open(path.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0644);
  if (fd == -1) {
    return false;
  }
  if (fd != target_fd) {
    if (::dup2(fd, target_fd) == -1) {
      while (::close(fd) == -1 && errno == EINTR) {
      }
      return false;
    }
    while (::close(fd) == -1 && errno == EINTR) {
    }
  }
  return true;
}

struct PipePair {
  int read_end;
  int write_end;
};

auto CreatePipeOrThrow() -> PipePair {
  int fds[2];
  if (::pipe(fds) == -1) {
    throw std::system_error(errno, std::generic_category(), "pipe");
  }
  return {fds[0], fds[1]};
}

}  // namespace

struct InputStreamProcess::Impl {
  Impl(pid_t child_pid, int stdin_fd)
      : pid(child_pid), buffer(stdin_fd), stream(&buffer) {}

  pid_t pid;
  FdOutputBuffer buffer;
  std::ostream stream;
  bool waited = false;
  int exit_code = -1;
};

InputStreamProcess::InputStreamProcess() = default;

InputStreamProcess::InputStreamProcess(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)) {}

InputStreamProcess::InputStreamProcess(InputStreamProcess&& other) noexcept =
    default;

auto InputStreamProcess::operator=(InputStreamProcess&& other) noexcept
    -> InputStreamProcess& = default;

InputStreamProcess::~InputStreamProcess() {
  if (!impl_) {
    return;
  }
  impl_->buffer.Close();
  if (!impl_->waited) {
    int status = 0;
    while (true) {
      pid_t result = ::waitpid(impl_->pid, &status, 0);
      if (result == -1 && errno == EINTR) {
        continue;
      }
      break;
    }
  }
}

std::ostream& InputStreamProcess::stdin() {
  if (!impl_) {
    throw std::runtime_error("process not initialized");
  }
  return impl_->stream;
}

auto InputStreamProcess::Wait() -> int {
  if (!impl_) {
    throw std::runtime_error("process not initialized");
  }
  if (impl_->waited) {
    return impl_->exit_code;
  }
  impl_->stream.flush();
  impl_->buffer.Close();
  impl_->stream.setstate(std::ios::badbit);

  int status = 0;
  while (true) {
    pid_t result = ::waitpid(impl_->pid, &status, 0);
    if (result == -1) {
      if (errno == EINTR) {
        continue;
      }
      throw std::system_error(errno, std::generic_category(), "waitpid");
    }
    break;
  }

  impl_->waited = true;
  if (WIFEXITED(status)) {
    impl_->exit_code = WEXITSTATUS(status);
  } else if (WIFSIGNALED(status)) {
    impl_->exit_code = 128 + WTERMSIG(status);
  } else {
    impl_->exit_code = -1;
  }

  return impl_->exit_code;
}

auto ResolveExecutable(const std::string& name) -> std::string {
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

auto RunCommand(const std::string& executable,
                const std::vector<std::string>& arguments,
                const RunOptions& options) -> RunResult {
  RunResult result;
  const std::string resolved = ResolveExecutable(executable);
  if (resolved.empty()) {
    return result;  // started == false
  }

  auto vectors = BuildExecVectors(resolved, arguments, options.env);

  const pid_t pid = ::fork();
  if (pid == -1) {
    return result;
  }

  if (pid == 0) {
    // Own process group, so a timeout can signal the whole tree. Build tools
    // fan out into workers that outlive their parent otherwise.
    ::setpgid(0, 0);
    if (!options.cwd.empty() && ::chdir(options.cwd.c_str()) == -1) {
      _exit(127);
    }
    if (!RedirectStream(
            options.stdout_path.empty() ? "/dev/null" : options.stdout_path,
            STDOUT_FILENO) ||
        !RedirectStream(
            options.stderr_path.empty() ? "/dev/null" : options.stderr_path,
            STDERR_FILENO)) {
      _exit(127);
    }
    if (options.address_space_limit_bytes > 0) {
      // Soft and hard together, so the child cannot raise it back.
      const rlim_t bytes =
          static_cast<rlim_t>(options.address_space_limit_bytes);
      const struct rlimit limit = {bytes, bytes};
      if (::setrlimit(RLIMIT_AS, &limit) == -1) {
        _exit(127);
      }
    }
    char* const* env_ptr = vectors.envp.empty() ? environ : vectors.envp.data();
    ::execve(vectors.argv[0], vectors.argv.data(), env_ptr);
    _exit(127);
  }

  // Also set from the parent: whichever runs first wins, and neither side may
  // assume the other has been scheduled yet.
  ::setpgid(pid, pid);
  if (options.on_started) {
    // The child is in its own group now, so a pgid kill from elsewhere reaches
    // the whole tree rather than racing the setpgid above.
    options.on_started(pid);
  }
  result.started = true;

  const auto deadline = std::chrono::steady_clock::now() + options.timeout;
  bool signalled = false;
  auto grace_deadline = std::chrono::steady_clock::time_point::max();

  for (;;) {
    int status = 0;
    const pid_t waited = ::waitpid(pid, &status, WNOHANG);
    if (waited == -1) {
      if (errno == EINTR) {
        continue;
      }
      return result;
    }
    if (waited == pid) {
      if (WIFEXITED(status)) {
        result.exit_code = WEXITSTATUS(status);
      } else if (WIFSIGNALED(status)) {
        result.exit_code = 128 + WTERMSIG(status);
      }
      // Reap anything else the group left behind.
      ::kill(-pid, SIGKILL);
      while (::waitpid(-pid, nullptr, WNOHANG) > 0) {
      }
      return result;
    }

    const auto now = std::chrono::steady_clock::now();
    if (options.timeout.count() > 0 && !signalled && now >= deadline) {
      result.timed_out = true;
      signalled = true;
      ::kill(-pid, SIGTERM);
      grace_deadline = now + options.kill_grace;
    } else if (signalled && now >= grace_deadline) {
      ::kill(-pid, SIGKILL);
      grace_deadline = std::chrono::steady_clock::time_point::max();
    }
    ::usleep(20000);
  }
}

auto CreateInputStreamProcess(
    const std::string& executable, const std::vector<std::string>& arguments,
    const std::vector<std::string>& env,
    std::filesystem::path const& stdout_path,
    std::filesystem::path const& stderr_path) -> InputStreamProcess {
  if (executable.empty()) {
    throw std::invalid_argument("executable path must not be empty");
  }

  auto vectors = BuildExecVectors(executable, arguments, env);
  PipePair pipe = CreatePipeOrThrow();

  pid_t pid = ::fork();
  if (pid == -1) {
    while (::close(pipe.read_end) == -1 && errno == EINTR) {
    }
    while (::close(pipe.write_end) == -1 && errno == EINTR) {
    }
    throw std::system_error(errno, std::generic_category(), "fork");
  }

  if (pid == 0) {
    while (::close(pipe.write_end) == -1 && errno == EINTR) {
    }
    if (::dup2(pipe.read_end, STDIN_FILENO) == -1) {
      _exit(127);
    }
    while (::close(pipe.read_end) == -1 && errno == EINTR) {
    }

    if (!RedirectStream(stdout_path, STDOUT_FILENO) ||
        !RedirectStream(stderr_path, STDERR_FILENO)) {
      _exit(127);
    }

    char* const* env_ptr = vectors.envp.empty() ? environ : vectors.envp.data();
    ::execve(vectors.argv[0], vectors.argv.data(), env_ptr);
    _exit(127);
  }

  while (::close(pipe.read_end) == -1 && errno == EINTR) {
  }

  if (::fcntl(pipe.write_end, F_SETFD, FD_CLOEXEC) == -1) {
    while (::close(pipe.write_end) == -1 && errno == EINTR) {
    }
    ::kill(pid, SIGKILL);
    while (::waitpid(pid, nullptr, 0) == -1 && errno == EINTR) {
    }
    throw std::system_error(errno, std::generic_category(), "fcntl");
  }

  auto impl = std::make_unique<InputStreamProcess::Impl>(pid, pipe.write_end);
  return InputStreamProcess(std::move(impl));
}

}  // namespace process