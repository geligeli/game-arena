#ifndef RISK_GAME_AI_CPP_TOURNAMENT_SERVER_GAME_HISTORY_H
#define RISK_GAME_AI_CPP_TOURNAMENT_SERVER_GAME_HISTORY_H

// Persists completed games: one binary GameRecord proto per game under
// <dir>/games/<game_id>.pb, plus one JSON line per game appended to
// <dir>/games/index.jsonl (read back by the HTTP leaderboard's /api/games).

#include <cstddef>
#include <deque>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

#include "cpp/tournament_server/tournament_broker.pb.h"

namespace tournament_broker {

class GameHistory {
 public:
  // How many index lines are kept in memory to serve RecentGames(). The
  // leaderboard asks for far fewer; the slack just avoids re-reading the file
  // if that ever changes.
  static constexpr std::size_t kRecentCapacity = 512;

  explicit GameHistory(std::filesystem::path dir);

  // Writes the record and appends to the index. Returns the record file path,
  // or an empty path on failure (logged).
  auto Store(const proto::GameRecord &record) -> std::filesystem::path;

  // Last |limit| index lines, oldest first.
  auto RecentGames(int limit) const -> std::vector<std::string>;

  auto dir() const -> const std::filesystem::path & { return dir_; }

 private:
  const std::filesystem::path dir_;  // <data_dir>/games
  mutable std::mutex mutex_;         // serializes index appends
  // Tail of index.jsonl, kept in memory so the leaderboard never re-reads the
  // whole file while holding the same lock that finishing games append under.
  // Seeded once at construction.
  std::deque<std::string> recent_;
};

}  // namespace tournament_broker

#endif  // RISK_GAME_AI_CPP_TOURNAMENT_SERVER_GAME_HISTORY_H
