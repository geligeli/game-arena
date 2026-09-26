#ifndef GAME_ARENA_GAME_ARENA_SERVER_JOB_LOG_H
#define GAME_ARENA_GAME_ARENA_SERVER_JOB_LOG_H

// Every job the coordinator ran, one JobRecord per file: <dir>/<job_id>.pb.
// The scheduler writes it as a job moves; the dashboard reads it for build
// output and submission history, which nothing else keeps.

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "game_arena/server/job_record.pb.h"

namespace tournament_arena {

class JobLog {
 public:
  explicit JobLog(std::filesystem::path dir);

  // Replaces the job's record. A failure is logged: a record the dashboard
  // misses is no reason to fail the job.
  void Put(const JobRecord &record);
  // Nullopt for an unknown job, or an id that cannot name a file.
  std::optional<JobRecord> Get(const std::string &job_id) const;
  // Every record, newest first.
  std::vector<JobRecord> List() const;

 private:
  const std::filesystem::path dir_;
};

}  // namespace tournament_arena

#endif  // GAME_ARENA_GAME_ARENA_SERVER_JOB_LOG_H
