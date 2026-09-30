#include "game_arena/standings/game_history.h"

#include <algorithm>
#include <boost/json/object.hpp>
#include <boost/json/serialize.hpp>
#include <boost/json/value_from.hpp>
#include <fstream>
#include <string>
#include <utility>

#include "absl/log/log.h"
#include "absl/strings/ascii.h"
#include "absl/strings/str_cat.h"

namespace tournament_broker {

namespace json = boost::json;

bool IsSafeId(std::string_view id) {
  return !id.empty() && std::ranges::all_of(id, [](char c) {
    return absl::ascii_isalnum(static_cast<unsigned char>(c)) || c == '_' ||
           c == '-';
  });
}

bool WriteAtomically(const std::filesystem::path &path, std::string_view bytes,
                     std::string *error) {
  const std::filesystem::path tmp = path.string() + ".tmp";
  if (!(std::ofstream(tmp, std::ios::binary) << bytes)) {
    *error = absl::StrCat("cannot write ", tmp.string());
    return false;
  }
  std::error_code ec;
  std::filesystem::rename(tmp, path, ec);
  if (ec) {
    *error = absl::StrCat("cannot replace ", path.string(), ": ", ec.message());
  }
  return !ec;
}

GameHistory::GameHistory(std::filesystem::path dir) : dir_(std::move(dir)) {
  std::error_code ec;
  std::filesystem::create_directories(dir_, ec);
  if (ec) {
    LOG(ERROR) << "Cannot create game history dir " << dir_ << ": "
               << ec.message();
  }
  // Seed the tail from what a previous run left behind.
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

std::vector<int> PlacesOf(const proto::GameRecord &record) {
  if (record.places_size() > 0) {
    return {record.places().begin(), record.places().end()};
  }
  const int seats = record.player_names_size();
  if (record.result() != proto::GameRecord::WIN) {
    return std::vector<int>(seats, 0);
  }
  std::vector<int> places(seats, 1);
  places[record.winning_player()] = 0;
  return places;
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

  // serialize() escapes newlines, so an entry stays on one line whatever the
  // names.
  json::object entry{
      {"game_id", record.game_id()},
      {"game", record.game()},
  };
  for (int seat = 0; seat < record.player_names_size(); ++seat) {
    entry["player" + std::to_string(seat)] = record.player_names(seat);
  }
  entry["result"] = static_cast<int>(record.result());
  entry["winning_player"] = record.winning_player();
  entry["places"] = json::value_from(PlacesOf(record));
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

std::vector<std::string> GameHistory::AllGames() const {
  std::lock_guard lock(mutex_);
  std::vector<std::string> lines;
  std::ifstream in(dir_ / "index.jsonl");
  for (std::string line; std::getline(in, line);) {
    if (!line.empty()) {
      lines.push_back(std::move(line));
    }
  }
  return lines;
}

std::optional<proto::GameRecord> GameHistory::Load(
    std::string_view game_id) const {
  if (!IsSafeId(game_id)) {
    return std::nullopt;
  }
  std::ifstream in(dir_ / (std::string(game_id) + ".pb"), std::ios::binary);
  proto::GameRecord record;
  if (!in || !record.ParseFromIstream(&in)) {
    return std::nullopt;
  }
  return record;
}

}  // namespace tournament_broker
