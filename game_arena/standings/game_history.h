#ifndef GAME_ARENA_GAME_ARENA_STANDINGS_GAME_HISTORY_H
#define GAME_ARENA_GAME_ARENA_STANDINGS_GAME_HISTORY_H

// Finished games: <dir>/<game_id>.pb each, and a line in <dir>/index.jsonl.

#include <cstddef>
#include <deque>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "game_arena/proto/tournament_broker.pb.h"

namespace tournament_broker {

// True when |id| can name a file: non-empty, [A-Za-z0-9_-]. Game and job ids
// reach the coordinator's pages from URLs.
bool IsSafeId(std::string_view id);

// Writes |bytes| to a temp file renamed over |path|, so a reader never sees
// half a file. On failure, false with *error set.
bool WriteAtomically(const std::filesystem::path &path, std::string_view bytes,
                     std::string *error);

class GameHistory {
 public:
  // Index lines kept in memory for RecentGames().
  static constexpr std::size_t kRecentCapacity = 512;

  explicit GameHistory(std::filesystem::path dir);

  // The record's path, or empty on failure (logged).
  std::filesystem::path Store(const proto::GameRecord &record);

  // Last |limit| index lines, oldest first.
  std::vector<std::string> RecentGames(int limit) const;
  // Every index line, oldest first, read from disk.
  std::vector<std::string> AllGames() const;
  // Nullopt when there is none, or |game_id| cannot name a file.
  std::optional<proto::GameRecord> Load(std::string_view game_id) const;

 private:
  const std::filesystem::path dir_;  // <data_dir>/games
  mutable std::mutex mutex_;         // serializes index appends
  // Tail of index.jsonl, so the leaderboard never re-reads it under mutex_.
  std::deque<std::string> recent_;
};

}  // namespace tournament_broker

#endif  // GAME_ARENA_GAME_ARENA_STANDINGS_GAME_HISTORY_H
