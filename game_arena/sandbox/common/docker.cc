#include "game_arena/sandbox/common/docker.h"

#include <cctype>
#include <utility>

namespace sandbox_common {

std::string ShellQuote(const std::string &value) {
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

std::string SanitizeContainerName(const std::string &value) {
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

std::string BindMount(const std::filesystem::path &source, const std::string &target,
               bool readonly) {
  std::string mount =
      "type=bind,source=" + source.string() + ",target=" + target;
  if (readonly) {
    mount += ",readonly";
  }
  return mount;
}

std::string VolumeMount(const std::string &volume, const std::string &target,
                 bool readonly) {
  std::string mount = "type=volume,source=" + volume + ",target=" + target;
  if (readonly) {
    mount += ",readonly";
  }
  return mount;
}

std::string ScratchPrelude() {
  return "export HOME=" + std::string(kScratch) + "\n";
}

StepResult CreateVolume(const std::string &docker, const std::string &name,
                  const std::filesystem::path &log_dir,
                  const std::string &tag) {
  return RunStep(docker, {"volume", "create", name}, /*cwd=*/{}, log_dir, tag,
                 std::chrono::seconds(60));
}

process::RunResult RemoveVolume(const std::string &docker,
                  const std::string &name) {
  process::RunOptions options;
  options.timeout = std::chrono::seconds(60);
  return process::RunCommand(docker, {"volume", "rm", "-f", name}, options);
}

process::RunResult KillContainer(const std::string &docker,
                   const std::string &name) {
  // The client-side wait may already have been stopped by a timeout or a
  // cancel; the container itself is the daemon's and would otherwise keep
  // running.
  process::RunOptions options;
  options.timeout = std::chrono::seconds(60);
  return process::RunCommand(docker, {"kill", name}, options);
}

process::RunResult RemoveContainer(const std::string &docker,
                     const std::string &name) {
  process::RunOptions options;
  options.timeout = std::chrono::seconds(60);
  return process::RunCommand(docker, {"rm", "-f", name}, options);
}

StepResult WaitForContainer(const std::string &docker, const std::string &name,
                      std::chrono::seconds timeout,
                      const std::filesystem::path &log_dir,
                      const std::string &tag) {
  return RunStep(docker, {"wait", name}, /*cwd=*/{}, log_dir, tag, timeout);
}

StepResult ContainerLogs(const std::string &docker, const std::string &name,
                   const std::filesystem::path &log_dir,
                   const std::string &tag) {
  return RunStep(docker, {"logs", name}, /*cwd=*/{}, log_dir, tag,
                 std::chrono::seconds(60));
}

StepResult CreateInternalNetwork(const std::string &docker, const std::string &name,
                           const std::filesystem::path &log_dir,
                           const std::string &tag) {
  return RunStep(docker, {"network", "create", "--internal", name},
                 /*cwd=*/{}, log_dir, tag, std::chrono::seconds(60));
}

process::RunResult RemoveNetwork(const std::string &docker,
                   const std::string &name) {
  process::RunOptions options;
  options.timeout = std::chrono::seconds(60);
  return process::RunCommand(docker, {"network", "rm", name}, options);
}

std::vector<std::string> DockerRunArgs(const DockerRunSpec &spec) {
  std::vector<std::string> args = {spec.create ? "create" : "run"};
  if (spec.rm && !spec.create) {
    args.push_back("--rm");
  }
  args.insert(args.end(), {"--name", spec.name});
  if (spec.detached && !spec.create) {
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
