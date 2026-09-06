#include "game_arena/server/metric_standings.h"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <ios>
#include <string>
#include <utility>

#include "absl/log/log.h"

namespace tournament_arena {

namespace {

auto NowUnixMs() -> int64_t {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

}  // namespace

MetricStandings::MetricStandings(std::filesystem::path path,
                                 std::string metric_name, bool lower_is_better)
    : path_(std::move(path)),
      metric_name_(std::move(metric_name)),
      lower_is_better_(lower_is_better) {}

void MetricStandings::Load() {
  std::ifstream in(path_, std::ios::binary);
  if (!in) {
    return;  // missing file = empty store
  }
  std::lock_guard lock(mutex_);
  if (!store_.ParseFromIstream(&in)) {
    LOG(ERROR) << "Cannot parse metric store " << path_
               << "; starting from empty";
    store_.Clear();
  }
}

void MetricStandings::Record(const std::string &candidate_id,
                             const std::string & /*opponent*/,
                             const proto::OrderResult &result) {
  if (result.metrics().empty()) {
    LOG(WARNING) << "Graded order for " << candidate_id
                 << " reported no metrics; nothing to rank it by";
    return;
  }

  std::string blob;
  uint64_t version = 0;
  {
    std::lock_guard lock(mutex_);
    proto::MetricRecord &record = (*store_.mutable_records())[candidate_id];
    record.set_candidate_id(candidate_id);
    // Replace rather than merge: the newest measurement is the one that stands,
    // and keeping a stale metric alongside a fresh one would rank a submission
    // on numbers taken at different times.
    record.mutable_metrics()->clear();
    for (const auto &[name, value] : result.metrics()) {
      (*record.mutable_metrics())[name] = value;
    }
    record.set_worker_id(result.worker_id());
    record.set_machine_class(result.machine_class());
    record.set_measured_unix_ms(NowUnixMs());
    record.set_runs(result.games_played());
    version = ++version_;
    blob = store_.SerializeAsString();
  }
  Save(blob, version);
}

void MetricStandings::Save(const std::string &blob, uint64_t version) {
  std::lock_guard lock(save_mutex_);
  if (version <= saved_version_) {
    return;  // a newer store already landed
  }
  const std::filesystem::path tmp = path_.string() + ".tmp";
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!out.write(blob.data(), static_cast<std::streamsize>(blob.size()))) {
      LOG(ERROR) << "Cannot write metric store " << tmp;
      return;
    }
  }
  std::error_code ec;
  std::filesystem::rename(tmp, path_, ec);
  if (ec) {
    LOG(ERROR) << "Cannot rename " << tmp << " to " << path_ << ": "
               << ec.message();
    return;
  }
  saved_version_ = version;
}

auto MetricStandings::has(const std::string &candidate_id) const -> bool {
  std::lock_guard lock(mutex_);
  return store_.records().contains(candidate_id);
}

auto MetricStandings::Get(const std::string &candidate_id) const -> Standing {
  std::lock_guard lock(mutex_);
  Standing standing;
  standing.candidate_id = candidate_id;
  const auto it = store_.records().find(candidate_id);
  if (it == store_.records().end()) {
    return standing;
  }
  for (const auto &[name, value] : it->second.metrics()) {
    standing.metrics[name] = value;
  }
  standing.worker_id = it->second.worker_id();
  standing.machine_class = it->second.machine_class();
  standing.runs = it->second.runs();

  const auto primary = standing.metrics.find(metric_name_);
  if (primary != standing.metrics.end()) {
    standing.score = primary->second;
  }
  return standing;
}

auto MetricStandings::Rank(int limit) const -> std::vector<Standing> {
  std::vector<Standing> rows;
  {
    std::lock_guard lock(mutex_);
    for (const auto &[candidate_id, record] : store_.records()) {
      (void)record;
      rows.push_back(Standing{});
      rows.back().candidate_id = candidate_id;
    }
  }
  for (Standing &row : rows) {
    row = Get(row.candidate_id);
  }
  // A submission with no reading for the primary metric cannot be placed, so it
  // is left off rather than shown at one end as if it had scored there.
  std::erase_if(rows, [&](const Standing &row) {
    return !row.metrics.contains(metric_name_);
  });

  const bool lower_is_better = lower_is_better_;
  std::sort(rows.begin(), rows.end(),
            [&](const Standing &a, const Standing &b) {
              if (a.score != b.score) {
                return lower_is_better ? a.score < b.score : a.score > b.score;
              }
              return a.candidate_id < b.candidate_id;
            });
  if (limit > 0 && rows.size() > static_cast<std::size_t>(limit)) {
    rows.resize(limit);
  }
  return rows;
}

}  // namespace tournament_arena
