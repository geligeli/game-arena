#include "game_arena/standings/game_history.h"

#include <algorithm>
#include <boost/json/object.hpp>
#include <boost/json/serialize.hpp>
#include <fstream>
#include <string>
#include <utility>

#include "absl/log/log.h"

namespace tournament_broker {

namespace json = boost::json;

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

std::filesystem::path GameHistory::Store(const proto::GameRecord &record) {
  const std::filesystem::path path = dir_ / (record.game_id() + ".pb");
  {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out || !record.SerializeToOstream(&out)) {
      LOG(ERROR) << "Could not write game record " << path;
      return {};
    }
  }

  // One line of index.jsonl. serialize() never emits a newline of its own --
  // a name carrying one comes back as \n -- so the object stays on one line
  // however the players are called.
  json::object entry{
      {"game_id", record.game_id()},
      {"game", record.game()},
  };
  for (int seat = 0; seat < record.player_names_size(); ++seat) {
    entry["player" + std::to_string(seat)] = record.player_names(seat);
  }
  entry["result"] = static_cast<int>(record.result());
  entry["winning_player"] = record.winning_player();
  entry["reason"] = record.termination_reason();
  entry["moves"] = record.steps_size();
  entry["finished_unix_ms"] = record.finished_unix_ms();
  std::string line = json::serialize(entry);

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

std::vector<std::string> GameHistory::RecentGames(int limit) const {
  std::lock_guard lock(mutex_);
  if (limit <= 0) {
    return {};
  }
  const std::size_t count =
      std::min(recent_.size(), static_cast<std::size_t>(limit));
  return {recent_.end() - static_cast<std::ptrdiff_t>(count), recent_.end()};
}

}  // namespace tournament_broker
