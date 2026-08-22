#ifndef RISK_GAME_AI_CPP_TOURNAMENT_SERVER_ELO_STORE_H
#define RISK_GAME_AI_CPP_TOURNAMENT_SERVER_ELO_STORE_H

// Persistent per-(game, player) ELO ratings. Loaded from disk at startup,
// updated online after each finished game (standard E = 1/(1+10^((Rb-Ra)/400)),
// R += K*(S-E)), and rewritten atomically (tmp + rename) after every update.
//
// The rewrite happens *outside* mutex_ so a finishing game never blocks other
// games or the leaderboard on disk I/O; writers are serialized on save_mutex_
// and stamped with a version so a slow writer cannot clobber a newer store.

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <utility>

#include "cpp/tournament_server/tournament_broker.pb.h"

namespace tournament_broker {

class EloStore {
 public:
  explicit EloStore(std::filesystem::path path, double k_factor = 32.0,
                    double initial_rating = 1500.0);

  // Reads the store file if it exists; missing file = empty store.
  void Load();

  // Applies one game result and persists. |score_a| is from player A's
  // perspective: 1.0 win, 0.5 draw, 0.0 loss. Returns the new ratings
  // {a, b}. Thread-safe.
  auto RecordResult(const std::string &game, const std::string &player_a,
                    const std::string &player_b,
                    double score_a) -> std::pair<double, double>;

  auto Get(const std::string &game,
           const std::string &player) const -> proto::Rating;

  auto Snapshot() const -> proto::RatingStore;

 private:
  static auto Key(const std::string &game,
                  const std::string &player) -> std::string;
  // Atomic rewrite (tmp + rename) of an already-serialized store. Must be
  // called without mutex_ held; drops |blob| if a newer version already landed.
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

#endif  // RISK_GAME_AI_CPP_TOURNAMENT_SERVER_ELO_STORE_H
