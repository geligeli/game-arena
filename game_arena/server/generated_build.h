#ifndef GAME_ARENA_GAME_ARENA_SERVER_GENERATED_BUILD_H
#define GAME_ARENA_GAME_ARENA_SERVER_GENERATED_BUILD_H

// A structured submission's BUILD, generated so no submitter can add a genrule.

#include <string>
#include <vector>

#include "game_arena/proto/problem.pb.h"

namespace tournament_arena {

// Empty when unusable. Names no directory (package_name()): builds anywhere.
std::string GenerateCandidateBuild(const proto::CandidateHarness &harness,
                                   const std::vector<std::string> &file_paths,
                                   const std::string &entry_header,
                                   const std::vector<std::string> &extra_deps);

}  // namespace tournament_arena

#endif  // GAME_ARENA_GAME_ARENA_SERVER_GENERATED_BUILD_H
