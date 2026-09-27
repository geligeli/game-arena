#ifndef GAME_ARENA_GAME_ARENA_STANDINGS_ELO_STORE_H
#define GAME_ARENA_GAME_ARENA_STANDINGS_ELO_STORE_H

// Per-(game, player) ELO, rewritten after every update outside mutex_; the
// version keeps a slow writer from clobbering a newer store.

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <utility>

#include "game_arena/proto/tournament_broker.pb.h"

namespace tournament_broker {

class EloStore {
 public:
  explicit EloStore(std::filesystem::path path, double k_factor = 32.0,
                    double initial_rating = 1500.0);

  // Reads the store file if it exists; missing file = empty store.
  void Load();

  // |score_a| is 1.0, 0.5 or 0.0 for A's win, draw or loss. Returns {a, b}.
  std::pair<double, double> RecordResult(const std::string &game,
                                         const std::string &player_a,
                                         const std::string &player_b,
                                         double score_a);

  proto::Rating Get(const std::string &game, const std::string &player) const;

 private:
  static std::string Key(const std::string &game, const std::string &player);
  // Call without mutex_ held; drops |blob| if a newer version already landed.
  void Save(const std::string &blob, uint64_t version);

  const std::filesystem::path path_;
  const double k_factor_;
  const double initial_rating_;
  mutable std::mutex mutex_;
  proto::RatingStore store_;
  uint64_t version_ = 0;  // guarded by mutex_; bumped on every update

  std::mutex save_mutex_;  // serializes writers; never taken with mutex_ held
  uint64_t saved_version_ = 0;  // guarded by save_mutex_
};

}  // namespace tournament_broker

#endif  // GAME_ARENA_GAME_ARENA_STANDINGS_ELO_STORE_H
