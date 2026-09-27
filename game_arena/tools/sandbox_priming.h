#ifndef GAME_ARENA_GAME_ARENA_TOOLS_SANDBOX_PRIMING_H
#define GAME_ARENA_GAME_ARENA_TOOLS_SANDBOX_PRIMING_H

// A disk cache hits only for the build that filled it: these are the sandbox's.

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "game_arena/proto/problem.pb.h"

namespace tournament_arena {

// The targets a sandbox builds for |submission|; empty drops those naming one.
std::vector<std::string> PrimeTargets(const proto::ProblemConfig &config,
                                      std::string_view submission);

// |image_rc| rooted at |root|, then the workspace's rc: a sandbox's order.
std::string PrimeBazelrc(std::string_view image_rc,
                         const std::filesystem::path &root);

// tar's --owner/--group for run_as_user: "" is root; a lone user is its group.
std::vector<std::string> TarOwner(std::string_view run_as_user);

}  // namespace tournament_arena

#endif  // GAME_ARENA_GAME_ARENA_TOOLS_SANDBOX_PRIMING_H
