#include "game_arena/sandbox/common/docker.h"

#include <cctype>
#include <utility>

namespace sandbox_common {

auto ShellQuote(const std::string &value) -> std::string {
  std::string quoted = "'";
  for (const char c : value) {
    if (c == '\'') {
      quoted += "'\\''";
    } else {
      quoted += c;
    }
  }
  quoted += "'";
  return quoted;
}

auto SanitizeContainerName(const std::string &value) -> std::string {
  std::string name;
  for (const unsigned char c : value) {
    if (std::isalnum(c) != 0 || c == '_' || c == '.' || c == '-') {
      name += static_cast<char>(c);
    } else {
      name += '-';
    }
  }
  return name;
}

auto BindMount(const std::filesystem::path &source, const std::string &target,
               bool readonly) -> std::string {
  std::string mount =
      "type=bind,source=" + source.string() + ",target=" + target;
  if (readonly) {
    mount += ",readonly";
  }
  return mount;
}

auto ScratchSetupScript() -> std::string {
  return "mkdir -p " + std::string(kScratch) + "/upper " + kScratch + "/work " +
         kWorkspace +
         "\n"
         "export HOME=" +
         kScratch + "\n";
}

auto OverlayMountScript() -> std::string {
  return ScratchSetupScript() +
         "mount -t overlay overlay -o lowerdir=" + kLowerMount +
         ",upperdir=" + kScratch + "/upper,workdir=" + kScratch + "/work " +
         kWorkspace + "\n";
}

auto HostOverlayPrelude() -> std::string {
  return "export HOME=" + std::string(kScratch) + "\n";
}

auto KillContainer(const std::string &docker,
                   const std::string &name) -> process::RunResult {
  // The client-side wait may already have been stopped by a timeout or a
  // cancel; the container itself is the daemon's and would otherwise keep
  // running.
  process::RunOptions options;
  options.timeout = std::chrono::seconds(60);
  return process::RunCommand(docker, {"kill", name}, options);
}

auto RemoveContainer(const std::string &docker,
                     const std::string &name) -> process::RunResult {
  process::RunOptions options;
  options.timeout = std::chrono::seconds(60);
  return process::RunCommand(docker, {"rm", "-f", name}, options);
}

auto WaitForContainer(const std::string &docker, const std::string &name,
                      std::chrono::seconds timeout,
                      const std::filesystem::path &log_dir,
                      const std::string &tag) -> StepResult {
  return RunStep(docker, {"wait", name}, /*cwd=*/{}, log_dir, tag, timeout);
}

auto ContainerLogs(const std::string &docker, const std::string &name,
                   const std::filesystem::path &log_dir,
                   const std::string &tag) -> StepResult {
  return RunStep(docker, {"logs", name}, /*cwd=*/{}, log_dir, tag,
                 std::chrono::seconds(60));
}

auto CreateInternalNetwork(const std::string &docker, const std::string &name,
                           const std::filesystem::path &log_dir,
                           const std::string &tag) -> StepResult {
  return RunStep(docker, {"network", "create", "--internal", name},
                 /*cwd=*/{}, log_dir, tag, std::chrono::seconds(60));
}

auto RemoveNetwork(const std::string &docker,
                   const std::string &name) -> process::RunResult {
  process::RunOptions options;
  options.timeout = std::chrono::seconds(60);
  return process::RunCommand(docker, {"network", "rm", name}, options);
}

auto DockerRunArgs(const DockerRunSpec &spec) -> std::vector<std::string> {
  std::vector<std::string> args = {"run"};
  if (spec.rm) {
    args.push_back("--rm");
  }
  args.insert(args.end(), {"--name", spec.name});
  if (spec.detached) {
    args.push_back("-d");
  }
  for (const std::string &extra : spec.extra_args) {
    args.push_back(extra);
  }
  if (!spec.network.empty()) {
    args.insert(args.end(), {"--network", spec.network});
  }
  for (const std::string &mount : spec.mounts) {
    args.insert(args.end(), {"--mount", mount});
  }
  args.insert(args.end(),
              {"--entrypoint", "/bin/sh", spec.image, "-c", spec.script});
  return args;
}

}  // namespace sandbox_common
