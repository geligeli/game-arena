#include "game_arena/sandbox/exec/entrypoint.h"

#include <map>
#include <string>

#include "game_arena/sandbox/common/docker.h"

namespace sandbox_exec {

namespace {

using sandbox_common::ShellQuote;

auto PreludeFor(const proto::Workspace &workspace) -> std::string {
  switch (workspace.overlay()) {
    case proto::Workspace::OVERLAY_IN_SANDBOX:
      return sandbox_common::OverlayMountScript();
    case proto::Workspace::OVERLAY_HOST:
      return sandbox_common::HostOverlayPrelude();
    case proto::Workspace::OVERLAY_NONE:
      // Nothing to assemble, but bazel still insists on a writable HOME.
      return sandbox_common::ScratchSetupScript();
    default:
      return sandbox_common::HostOverlayPrelude();
  }
}

auto WorkDirOf(const proto::Workspace &workspace,
               const proto::Step &step) -> std::string {
  if (!step.cwd().empty()) {
    return step.cwd();
  }
  if (!workspace.sandbox_work_dir().empty()) {
    return workspace.sandbox_work_dir();
  }
  return sandbox_common::kWorkspace;
}

}  // namespace

auto RenderArgv(const proto::Step &step) -> std::string {
  std::string rendered;
  for (const proto::Token &token : step.argv()) {
    if (!rendered.empty()) {
      rendered += " ";
    }
    rendered += token.verbatim() ? token.text() : ShellQuote(token.text());
  }
  return rendered;
}

auto EntrypointScript(const proto::Workspace &workspace,
                      const proto::Step &step) -> std::string {
  std::string script = "set -eu\n";
  script += PreludeFor(workspace);

  if (step.applies_patches() &&
      workspace.patch() == proto::Workspace::PATCH_COPY_IN_ENTRYPOINT) {
    script += std::string("cp -a ") + sandbox_common::kPatchMount + "/. " +
              sandbox_common::kWorkspace + "/\n";
  }
  script += "cd " + WorkDirOf(workspace, step) + "\n";

  if (step.applies_patches() &&
      workspace.patch() == proto::Workspace::PATCH_IN_ENTRYPOINT) {
    for (const std::string &name : workspace.patch_files()) {
      script +=
          "git apply " +
          ShellQuote(std::string(sandbox_common::kPatchMount) + "/" + name) +
          "\n";
    }
  }
  // Sorted, because a map's order is not the caller's and a build that
  // differs only in the order of two exports is a cache miss for nothing.
  for (const auto &[key, value] : std::map<std::string, std::string>(
           step.env().begin(), step.env().end())) {
    script += "export " + key + "=" + ShellQuote(value) + "\n";
  }

  script += "exec " + RenderArgv(step) + "\n";
  return script;
}

}  // namespace sandbox_exec
