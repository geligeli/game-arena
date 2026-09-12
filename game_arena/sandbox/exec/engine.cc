#include "game_arena/sandbox/exec/engine.h"

#include <string>

#include "game_arena/sandbox/common/docker.h"

namespace sandbox_exec {

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
