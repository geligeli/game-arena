#include "game_arena/tools/sandbox_priming.h"

#include "absl/strings/str_cat.h"
#include "absl/strings/str_replace.h"
#include "absl/strings/str_split.h"
#include "game_arena/server/problem_config.h"

namespace tournament_arena {

std::vector<std::string> PrimeTargets(const proto::ProblemConfig &config,
                                      std::string_view submission) {
  std::vector<std::string> targets;
  for (const std::string &target : config.build().targets()) {
    if (submission.empty() &&
        target.find("{submission_id}") != std::string::npos) {
      continue;
    }
    targets.push_back(ExpandSubmissionId(target, submission));
  }
  if (!config.match().referee_target().empty()) {
    targets.push_back(
        ExpandSubmissionId(config.match().referee_target(), submission));
  }
  return targets;
}

std::string PrimeBazelrc(std::string_view image_rc,
                         const std::filesystem::path &root) {
  return absl::StrCat(
      absl::StrReplaceAll(image_rc,
                          {{"/opt/arena/", (root / "opt/arena/").string()}}),
      "\ntry-import %workspace%/.bazelrc\n");
}

std::vector<std::string> TarOwner(std::string_view run_as_user) {
  const std::vector<std::string> parts = absl::StrSplit(run_as_user, ':');
  const std::string user = parts[0].empty() ? "0" : parts[0];
  const std::string group = parts.size() > 1 ? parts[1] : user;
  return {"--owner=" + user, "--group=" + group};
}

}  // namespace tournament_arena
