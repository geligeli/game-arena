#ifndef GAME_ARENA_GAME_ARENA_SANDBOX_EXEC_CHECKOUT_H
#define GAME_ARENA_GAME_ARENA_SANDBOX_EXEC_CHECKOUT_H

// The git work a workspace needs before anything runs: clone once, then
// fetch and force-checkout the commit the job names.
//
// Always on the host, never in the sandbox -- so the tree a sandbox sees is
// already at the right commit and no sandbox needs credentials, a network, or
// write access to a .git directory.

#include <filesystem>
#include <string>

namespace sandbox_exec {

// Clones |source| into |dest| on first use -- an existing .git means the
// clone is already there. --local hardlinks the object store instead of
// copying it, so a clone of a multi-gigabyte history costs almost nothing on
// the same filesystem. Returns false with *error set.
auto EnsureClone(const std::string &git, const std::string &source,
                 const std::filesystem::path &dest,
                 const std::filesystem::path &log_dir,
                 std::string *error) -> bool;

// A best-effort fetch, then a forced checkout of |commit|. The fetch is
// allowed to fail: a stale mirror is survivable, and the checkout decides.
// An empty |commit| leaves the tree where it is. Returns false with *error
// set.
auto SyncToCommit(const std::string &git, const std::filesystem::path &repo,
                  const std::string &commit,
                  const std::filesystem::path &log_dir,
                  std::string *error) -> bool;

}  // namespace sandbox_exec

#endif  // GAME_ARENA_GAME_ARENA_SANDBOX_EXEC_CHECKOUT_H
