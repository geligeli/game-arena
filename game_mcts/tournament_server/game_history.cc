#include "cpp/tournament_server/game_history.h"

#include <algorithm>
#include <fstream>
#include <utility>

#include "absl/log/log.h"

namespace tournament_broker {

namespace {

// Minimal JSON string escaping (the fields written here are player names,
// game ids, and fixed vocabularies — no control characters expected, but
// quotes and backslashes are handled).
auto JsonEscape(const std::string &s) -> std::string {
  std::string out;
  out.reserve(s.size());
  for (const char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default: out += c;
    }
  }
  return out;
}

}  // namespace

GameHistory::GameHistory(std::filesystem::path dir) : dir_(std::move(dir)) {
  std::error_code ec;
  std::filesystem::create_directories(dir_, ec);
  if (ec) {
    LOG(ERROR) << "Cannot create game history dir " << dir_ << ": "
               << ec.message();
  }
  // Seed the in-memory tail from whatever a previous run left behind. This is
  // the only time the index file is read.
  std::ifstream in(dir_ / "index.jsonl");
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty()) {
      continue;
    }
    recent_.push_back(std::move(line));
    if (recent_.size() > kRecentCapacity) {
      recent_.pop_front();
    }
  }
}

auto GameHistory::Store(const proto::GameRecord &record)
    -> std::filesystem::path {
  const std::filesystem::path path = dir_ / (record.game_id() + ".pb");
  {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out || !record.SerializeToOstream(&out)) {
      LOG(ERROR) << "Could not write game record " << path;
      return {};
    }
  }

  std::string line = "{\"game_id\":\"" + JsonEscape(record.game_id()) +
                     "\",\"game\":\"" + JsonEscape(record.game()) + "\"";
  for (int seat = 0; seat < record.player_names_size(); ++seat) {
    line += ",\"player" + std::to_string(seat) + "\":\"" +
            JsonEscape(record.player_names(seat)) + "\"";
  }
  line += ",\"result\":" +
          std::to_string(static_cast<int>(record.result())) +
          ",\"winning_player\":" + std::to_string(record.winning_player()) +
          ",\"reason\":\"" + JsonEscape(record.termination_reason()) + "\"" +
          ",\"moves\":" + std::to_string(record.steps_size()) +
          ",\"finished_unix_ms\":" +
          std::to_string(record.finished_unix_ms()) + "}";

  std::lock_guard lock(mutex_);
  std::ofstream index(dir_ / "index.jsonl", std::ios::app);
  if (!index) {
    LOG(ERROR) << "Could not append to game index in " << dir_;
    return path;
  }
  index << line << '\n';
  recent_.push_back(std::move(line));
  if (recent_.size() > kRecentCapacity) {
    recent_.pop_front();
  }
  return path;
}

auto GameHistory::RecentGames(int limit) const -> std::vector<std::string> {
  std::lock_guard lock(mutex_);
  if (limit <= 0) {
    return {};
  }
  const std::size_t count =
      std::min(recent_.size(), static_cast<std::size_t>(limit));
  return {recent_.end() - static_cast<std::ptrdiff_t>(count), recent_.end()};
}

}  // namespace tournament_broker
