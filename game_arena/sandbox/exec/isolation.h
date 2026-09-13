#ifndef GAME_ARENA_GAME_ARENA_SANDBOX_EXEC_ISOLATION_H
#define GAME_ARENA_GAME_ARENA_SANDBOX_EXEC_ISOLATION_H

// Isolation, as docker flags.
//
// One function, and no path through the container engine that does not go
// through it. That is the difference between the isolation claims in
// game_arena/ARENA.md being a property of the code and being a paragraph in a
// header: before this, the hardening lived in one private method of one
// backend, and the standalone runner -- which also runs submitted code in a
// container -- simply did not call it.

#include <string>
#include <vector>

#include "game_arena/sandbox/exec/sandbox_job.pb.h"

namespace sandbox_exec {

// The flags for |isolation|, in a fixed order, ahead of the network and the
// mounts. Nothing here is optional for a caller to remember: the zero value
// of every relaxation is the hardened one, so a default-constructed Isolation
// produces --cap-drop ALL, no-new-privileges and a read-only root.
std::vector<std::string> IsolationArgs(const proto::Isolation &isolation);

// What to pass docker's --network for |isolation|, given the name of the
// phase's own bridge. Empty leaves docker's default, which is never what a
// sandbox wants and so is never returned here.
std::string NetworkArg(const proto::Isolation &isolation,
                       const std::string &phase_network);

// True when |isolation| asks for a bridge the engine has to create first.
bool NeedsPhaseNetwork(const proto::Isolation &isolation);

}  // namespace sandbox_exec

#endif  // GAME_ARENA_GAME_ARENA_SANDBOX_EXEC_ISOLATION_H
