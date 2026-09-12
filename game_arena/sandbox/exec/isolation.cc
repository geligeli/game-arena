#include "game_arena/sandbox/exec/isolation.h"

#include <string>
#include <vector>

namespace sandbox_exec {

auto IsolationArgs(const proto::Isolation &isolation)
    -> std::vector<std::string> {
  std::vector<std::string> args;
  if (!isolation.keep_default_caps()) {
    args.insert(args.end(), {"--cap-drop", "ALL"});
  }
  if (!isolation.allow_new_privileges()) {
    args.insert(args.end(), {"--security-opt", "no-new-privileges"});
  }
  if (!isolation.writable_rootfs()) {
    // Writable only where it must be. The overlay merge and the mounts stay
    // writable because they are bind mounts; everything else is not.
    args.emplace_back("--read-only");
  }
  for (const proto::Tmpfs &tmpfs : isolation.tmpfs()) {
    args.emplace_back("--tmpfs");
    args.emplace_back(tmpfs.options().empty()
                          ? tmpfs.target()
                          : tmpfs.target() + ":" + tmpfs.options());
  }
  // Granted on top of the drop above, which is strictly tighter than not
  // dropping at all.
  for (const std::string &capability : isolation.add_capabilities()) {
    args.insert(args.end(), {"--cap-add", capability});
  }
  if (!isolation.run_as_user().empty()) {
    args.insert(args.end(), {"--user", isolation.run_as_user()});
  }
  if (isolation.memory_limit_mb() > 0) {
    args.insert(
        args.end(),
        {"--memory", std::to_string(isolation.memory_limit_mb()) + "m"});
  }
  if (isolation.cpus() > 0.0) {
    args.insert(args.end(), {"--cpus", std::to_string(isolation.cpus())});
  }
  if (isolation.pids_limit() > 0) {
    args.insert(args.end(),
                {"--pids-limit", std::to_string(isolation.pids_limit())});
  }
  return args;
}

auto NetworkArg(const proto::Isolation &isolation,
                const std::string &phase_network) -> std::string {
  switch (isolation.network()) {
    case proto::Isolation::NETWORK_PHASE_BRIDGE:
      return phase_network;
    case proto::Isolation::NETWORK_EGRESS:
      return "bridge";
    case proto::Isolation::NETWORK_NONE:
    default:
      return "none";
  }
}

auto NeedsPhaseNetwork(const proto::Isolation &isolation) -> bool {
  return isolation.network() == proto::Isolation::NETWORK_PHASE_BRIDGE;
}

}  // namespace sandbox_exec
