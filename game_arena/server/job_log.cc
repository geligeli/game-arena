#include "game_arena/server/job_log.h"

#include <algorithm>
#include <fstream>
#include <functional>
#include <system_error>
#include <utility>

#include "absl/log/log.h"
#include "game_arena/standings/game_history.h"

namespace tournament_arena {

JobLog::JobLog(std::filesystem::path dir) : dir_(std::move(dir)) {
  std::error_code ec;
  std::filesystem::create_directories(dir_, ec);
}

void JobLog::Put(const JobRecord &record) {
  std::string error;
  if (!tournament_broker::WriteAtomically(
          dir_ / (record.job().job_id() + ".pb"), record.SerializeAsString(),
          &error)) {
    LOG(ERROR) << "Could not write job record: " << error;
  }
}

std::optional<JobRecord> JobLog::Get(const std::string &job_id) const {
  if (!tournament_broker::IsSafeId(job_id)) {
    return std::nullopt;
  }
  std::ifstream in(dir_ / (job_id + ".pb"), std::ios::binary);
  JobRecord record;
  if (!in || !record.ParseFromIstream(&in)) {
    return std::nullopt;
  }
  return record;
}

std::vector<JobRecord> JobLog::List() const {
  std::vector<JobRecord> records;
  std::error_code ec;
  for (const auto &entry : std::filesystem::directory_iterator(dir_, ec)) {
    if (entry.path().extension() != ".pb") {
      continue;
    }
    std::ifstream in(entry.path(), std::ios::binary);
    JobRecord record;
    if (record.ParseFromIstream(&in)) {
      records.push_back(std::move(record));
    }
  }
  std::ranges::sort(records, std::greater{}, [](const JobRecord &record) {
    return record.job().created_unix_ms();
  });
  return records;
}

}  // namespace tournament_arena
