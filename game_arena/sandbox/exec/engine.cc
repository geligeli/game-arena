#include "game_arena/sandbox/exec/engine.h"

#include <map>
#include <string>

#include "absl/strings/str_replace.h"
#include "game_arena/sandbox/common/docker.h"
#include "game_arena/sandbox/common/files.h"

namespace sandbox_exec {

proto::Step Substituted(
    const proto::Step &step,
    const std::map<std::string, std::string> &replacements) {
  // One key at a time, in key order: a value may itself hold a placeholder.
  const auto replace = [&](std::string *text) {
    for (const auto &[from, to] : replacements) {
      absl::StrReplaceAll({{from, to}}, text);
    }
  };
  proto::Step out = step;
  for (proto::Token &token : *out.mutable_argv()) {
    replace(token.mutable_text());
  }
  for (auto &[key, value] : *out.mutable_env()) {
    replace(&value);
  }
  return out;
}

bool Fail(proto::Status *status, proto::Status::Code code,
          const std::string &message) {
  status->set_code(code);
  status->set_message(message);
  return false;
}

proto::Isolation Merge(const proto::Isolation &base,
                       const proto::Isolation &override_with) {
  return override_with.ByteSizeLong() > 0 ? override_with : base;
}

proto::StepResult *AddStepResult(proto::PhaseResult *phase,
                                 const std::string &name,
                                 const std::filesystem::path &log_dir) {
  proto::StepResult *step = phase->add_steps();
  step->set_name(name);
  step->set_started(true);
  step->set_stdout(sandbox_common::ReadFile(log_dir / (name + ".out")));
  step->set_stderr(sandbox_common::ReadFile(log_dir / (name + ".err")));
  return step;
}

std::string SandboxName(const std::string &job_id,
                        const std::string &step_name) {
  const std::string base = sandbox_common::SanitizeContainerName(job_id);
  if (step_name.empty()) {
    return base;
  }
  return base + "-" + sandbox_common::SanitizeContainerName(step_name);
}

}  // namespace sandbox_exec
