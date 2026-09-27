#include "game_arena/sandbox/common/docker.h"

#include <cctype>

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

std::string BindMount(const std::filesystem::path &source,
                      const std::string &target, bool readonly) {
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
