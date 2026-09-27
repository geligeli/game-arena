#ifndef GAME_ARENA_GAME_ARENA_SERVER_JOB_LOG_H
#define GAME_ARENA_GAME_ARENA_SERVER_JOB_LOG_H

// Every job the coordinator ran, one JobRecord per <dir>/<job_id>.pb.

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "game_arena/server/job_record.pb.h"

namespace tournament_arena {

class JobLog {
 public:
  explicit JobLog(std::filesystem::path dir);

  // A failure is only logged: a missed record is no reason to fail the job.
  void Put(const JobRecord &record);
  // Nullopt for an unknown job, or an id that cannot name a file.
  std::optional<JobRecord> Get(const std::string &job_id) const;
  // Newest first.
  std::vector<JobRecord> List() const;

 private:
  const std::filesystem::path dir_;
};

}  // namespace tournament_arena

#endif  // GAME_ARENA_GAME_ARENA_SERVER_JOB_LOG_H
