#include "game_mcts/tournament_server/elo_store.h"

#include <cmath>
#include <fstream>

#include "absl/log/log.h"

namespace tournament_broker {

EloStore::EloStore(std::filesystem::path path, double k_factor,
                   double initial_rating)
    : path_(std::move(path)),
      k_factor_(k_factor),
      initial_rating_(initial_rating) {}

auto EloStore::Key(const std::string &game, const std::string &player)
    -> std::string {
  return game + "\t" + player;
}

void EloStore::Load() {
  std::lock_guard lock(mutex_);
  std::ifstream in(path_, std::ios::binary);
  if (!in) {
    return;  // No store yet.
  }
  if (!store_.ParseFromIstream(&in)) {
    LOG(ERROR) << "Could not parse rating store " << path_
               << "; starting from an empty store";
    store_.Clear();
  }
}

auto EloStore::RecordResult(const std::string &game,
                            const std::string &player_a,
                            const std::string &player_b, double score_a)
    -> std::pair<double, double> {
  std::pair<double, double> new_ratings;
  std::string blob;
  uint64_t version = 0;
  {
    std::lock_guard lock(mutex_);
    proto::Rating &a = (*store_.mutable_ratings())[Key(game, player_a)];
    proto::Rating &b = (*store_.mutable_ratings())[Key(game, player_b)];
    if (a.elo() == 0.0) a.set_elo(initial_rating_);
    if (b.elo() == 0.0) b.set_elo(initial_rating_);

    const double expected_a =
        1.0 / (1.0 + std::pow(10.0, (b.elo() - a.elo()) / 400.0));
    const double change = k_factor_ * (score_a - expected_a);
    a.set_elo(a.elo() + change);
    b.set_elo(b.elo() - change);

    if (score_a == 1.0) {
      a.set_wins(a.wins() + 1);
      b.set_losses(b.losses() + 1);
    } else if (score_a == 0.0) {
      a.set_losses(a.losses() + 1);
      b.set_wins(b.wins() + 1);
    } else {
      a.set_draws(a.draws() + 1);
      b.set_draws(b.draws() + 1);
    }

    new_ratings = {a.elo(), b.elo()};
    blob = store_.SerializeAsString();
    version = ++version_;
  }
  // Disk I/O outside mutex_: readers (leaderboard) and other finishing games
  // are not blocked by this rewrite.
  Save(blob, version);
  return new_ratings;
}

auto EloStore::Get(const std::string &game, const std::string &player) const
    -> proto::Rating {
  std::lock_guard lock(mutex_);
  const auto it = store_.ratings().find(Key(game, player));
  if (it == store_.ratings().end()) {
    proto::Rating rating;
    rating.set_elo(initial_rating_);
    return rating;
  }
  return it->second;
}

auto EloStore::Snapshot() const -> proto::RatingStore {
  std::lock_guard lock(mutex_);
  return store_;
}

void EloStore::Save(const std::string &blob, uint64_t version) {
  std::lock_guard lock(save_mutex_);
  if (version <= saved_version_) {
    // A newer store already reached disk; this blob is stale.
    return;
  }
  const std::filesystem::path tmp = path_.string() + ".tmp";
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!out || !out.write(blob.data(), static_cast<std::streamsize>(
                                            blob.size()))) {
      LOG(ERROR) << "Could not write rating store " << tmp;
      return;
    }
  }
  std::error_code ec;
  std::filesystem::rename(tmp, path_, ec);
  if (ec) {
    LOG(ERROR) << "Could not rename " << tmp << " -> " << path_ << ": "
               << ec.message();
    return;
  }
  saved_version_ = version;
}

}  // namespace tournament_broker
