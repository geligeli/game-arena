#include "game_arena/sandbox/exec/engine.h"

#include <cstddef>
#include <map>
#include <string>

#include "game_arena/sandbox/common/docker.h"

namespace sandbox_exec {

namespace {

auto Replaced(std::string text, const std::string &from,
              const std::string &to) -> std::string {
  if (from.empty()) {
    return text;
  }
  for (std::size_t at = text.find(from); at != std::string::npos;
       at = text.find(from, at + to.size())) {
    text.replace(at, from.size(), to);
  }
  return text;
}

}  // namespace

auto Substituted(const proto::Step &step,
                 const std::map<std::string, std::string> &replacements)
    -> proto::Step {
  proto::Step out = step;
  for (proto::Token &token : *out.mutable_argv()) {
    std::string text = token.text();
    for (const auto &[from, to] : replacements) {
      text = Replaced(text, from, to);
    }
    token.set_text(text);
  }
  for (auto &[key, value] : *out.mutable_env()) {
    for (const auto &[from, to] : replacements) {
      value = Replaced(value, from, to);
    }
  }
  return out;
}

auto SandboxName(const std::string &job_id,
                 const std::string &step_name) -> std::string {
  // A one-step job names its sandbox exactly after itself, which is what the
  // standalone runner has always done. A multi-step one suffixes, which is
  // what lets Cancel find every sandbox of a job it holds without keeping a
  // list of what it started.
  const std::string base = sandbox_common::SanitizeContainerName(job_id);
  if (step_name.empty()) {
    return base;
  }
  return base + "-" + sandbox_common::SanitizeContainerName(step_name);
}

}  // namespace sandbox_exec
