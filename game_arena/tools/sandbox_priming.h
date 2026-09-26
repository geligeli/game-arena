#ifndef GAME_ARENA_GAME_ARENA_TOOLS_SANDBOX_PRIMING_H
#define GAME_ARENA_GAME_ARENA_TOOLS_SANDBOX_PRIMING_H

// What `arena_tournament sandbox` needs to prime a build on this host that a
// sandbox then repeats: the targets, the rc and the owner of what it ships.
// A disk cache hits only for the build that filled it, so each of these is the
// sandbox's own, not a lookalike.

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "game_arena/proto/problem.pb.h"

namespace tournament_arena {

// build.targets and match.referee_target as a sandbox builds them for
// |submission|. Empty: the targets that name a submission are left out.
std::vector<std::string> PrimeTargets(const proto::ProblemConfig &config,
                                      std::string_view submission);

// The image's system rc (|image_rc|) for a build on this host: its paths under
// |root| rather than /, then the workspace's own rc -- with --noworkspace_rc,
// the order a sandbox reads the two in.
std::string PrimeBazelrc(std::string_view image_rc,
                         const std::filesystem::path &root);

// tar's --owner and --group for a sandbox's run_as_user: "" is root, and a
// user alone is its own group.
std::vector<std::string> TarOwner(std::string_view run_as_user);

}  // namespace tournament_arena

#endif  // GAME_ARENA_GAME_ARENA_TOOLS_SANDBOX_PRIMING_H
