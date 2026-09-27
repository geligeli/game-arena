#ifndef GAME_ARENA_GAME_ARENA_SANDBOX_EXEC_ISOLATION_H
#define GAME_ARENA_GAME_ARENA_SANDBOX_EXEC_ISOLATION_H

// Isolation as docker flags; no `docker run` is built without IsolationArgs.

#include <string>
#include <vector>

#include "game_arena/sandbox/exec/sandbox_job.pb.h"

namespace sandbox_exec {

// A default Isolation yields --cap-drop ALL, no-new-privileges, --read-only.
std::vector<std::string> IsolationArgs(const proto::Isolation &isolation);

// |phase_network| for a phase bridge; never docker's default network.
std::string NetworkArg(const proto::Isolation &isolation,
                       const std::string &phase_network);

bool NeedsPhaseNetwork(const proto::Isolation &isolation);

}  // namespace sandbox_exec

#endif  // GAME_ARENA_GAME_ARENA_SANDBOX_EXEC_ISOLATION_H
