#ifndef GAME_MCTS_GAME_MCTS_TOURNAMENT_SERVER_SANDBOX_WORKER_CHECKOUT_H
#define GAME_MCTS_GAME_MCTS_TOURNAMENT_SERVER_SANDBOX_WORKER_CHECKOUT_H

// The host-side git work both backends do per slot: clone once, then fetch
// and force-checkout the order's base commit. Shared so the two backends
// cannot drift into running the same order on different trees.

#include <filesystem>
#include <string>

namespace tournament_arena {

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

}  // namespace tournament_arena

#endif  // GAME_MCTS_GAME_MCTS_TOURNAMENT_SERVER_SANDBOX_WORKER_CHECKOUT_H
