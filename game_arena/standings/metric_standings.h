#ifndef GAME_ARENA_GAME_ARENA_STANDINGS_METRIC_STANDINGS_H
#define GAME_ARENA_GAME_ARENA_STANDINGS_METRIC_STANDINGS_H

// Standings for a graded problem, persisted like EloStore. Each row keeps its
// worker and machine class: a wall-clock number depends on the host.

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

#include "game_arena/standings/standings.h"

namespace tournament_arena {

class MetricStandings final : public Standings {
 public:
  // Plain values, not a MetricSpec, so this does not depend on problem.proto.
  MetricStandings(std::filesystem::path path, std::string metric_name,
                  bool lower_is_better);

  // Reads the store file if it exists; a missing file is an empty store.
  void Load();

  void Record(const std::string &candidate_id, const std::string &opponent,
              const proto::OrderResult &result) override;
  Standing Get(const std::string &candidate_id) const override;
  std::vector<Standing> Rank(int limit) const override;
  std::string score_label() const override { return metric_name_; }
  bool has(const std::string &candidate_id) const override;

 private:
  // Call without mutex_ held; drops |blob| if a newer version already landed.
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

#endif  // GAME_ARENA_GAME_ARENA_STANDINGS_METRIC_STANDINGS_H
