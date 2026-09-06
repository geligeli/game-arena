#ifndef GAME_ARENA_GAME_ARENA_SERVER_METRIC_STANDINGS_H
#define GAME_ARENA_GAME_ARENA_SERVER_METRIC_STANDINGS_H

// Standings for a graded problem: a measured number per submission, ranked by
// the problem's primary metric.
//
// Persisted the same way EloStore is -- loaded once at startup, rewritten
// atomically (tmp + rename) after every update, with the rewrite outside the
// lock so a finishing order never blocks the leaderboard on disk I/O.
//
// The worker and machine class are stored beside the numbers because a
// wall-clock measurement is not a property of the submission alone. A board
// that forgot which host produced each row would be ranking the fleet.

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

#include "game_arena/server/standings.h"

namespace tournament_arena {

class MetricStandings final : public Standings {
 public:
  // |metric_name| selects the number the board is ordered by, and
  // |lower_is_better| decides which end wins. Taken as plain values rather than
  // a MetricSpec so this does not depend on the problem-config schema.
  MetricStandings(std::filesystem::path path, std::string metric_name,
                  bool lower_is_better);

  // Reads the store file if it exists; a missing file is an empty store.
  void Load();

  void Record(const std::string &candidate_id, const std::string &opponent,
              const proto::OrderResult &result) override;
  auto Get(const std::string &candidate_id) const -> Standing override;
  auto Rank(int limit) const -> std::vector<Standing> override;
  auto score_label() const -> std::string override { return metric_name_; }
  auto has(const std::string &candidate_id) const -> bool override;

 private:
  // Atomic rewrite of an already-serialized store. Called without mutex_ held;
  // drops |blob| if a newer version already landed.
  void Save(const std::string &blob, uint64_t version);

  const std::filesystem::path path_;
  const std::string metric_name_;
  const bool lower_is_better_;

  mutable std::mutex mutex_;
  proto::MetricStore store_;
  uint64_t version_ = 0;  // guarded by mutex_; bumped on every update

  std::mutex save_mutex_;       // serializes writers; never taken with mutex_
  uint64_t saved_version_ = 0;  // guarded by save_mutex_
};

}  // namespace tournament_arena

#endif  // GAME_ARENA_GAME_ARENA_SERVER_METRIC_STANDINGS_H
