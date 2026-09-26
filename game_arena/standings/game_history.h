#ifndef GAME_ARENA_GAME_ARENA_STANDINGS_GAME_HISTORY_H
#define GAME_ARENA_GAME_ARENA_STANDINGS_GAME_HISTORY_H

// Persists completed games: one binary GameRecord proto per game under
// <dir>/games/<game_id>.pb, plus one JSON line per game appended to
// <dir>/games/index.jsonl (read back by the HTTP leaderboard's /api/games).

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

class GameHistory {
 public:
  // How many index lines are kept in memory to serve RecentGames(). The
  // leaderboard asks for far fewer; the slack just avoids re-reading the file
  // if that ever changes.
  static constexpr std::size_t kRecentCapacity = 512;

  explicit GameHistory(std::filesystem::path dir);

  // Writes the record and appends to the index. Returns the record file path,
  // or an empty path on failure (logged).
  std::filesystem::path Store(const proto::GameRecord &record);

  // Last |limit| index lines, oldest first.
  std::vector<std::string> RecentGames(int limit) const;
  // Every index line, oldest first, read from disk: the in-memory tail holds
  // only the last kRecentCapacity.
  std::vector<std::string> AllGames() const;
  // One game's record. Nullopt when there is none, or |game_id| cannot name a
  // file.
  std::optional<proto::GameRecord> Load(std::string_view game_id) const;

 private:
  const std::filesystem::path dir_;  // <data_dir>/games
  mutable std::mutex mutex_;         // serializes index appends
  // Tail of index.jsonl, kept in memory so the leaderboard never re-reads the
  // whole file while holding the same lock that finishing games append under.
  // Seeded once at construction.
  std::deque<std::string> recent_;
};

}  // namespace tournament_broker

#endif  // GAME_ARENA_GAME_ARENA_STANDINGS_GAME_HISTORY_H
