#ifndef GAME_ARENA_GAME_ARENA_SERVER_CANDIDATE_STORE_H
#define GAME_ARENA_GAME_ARENA_SERVER_CANDIDATE_STORE_H

// Submissions on disk; a file list becomes a patch here, the only form after.

#include <cstddef>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "game_arena/proto/arena.pb.h"
#include "game_arena/proto/problem.pb.h"
#include "game_arena/standings/candidate_view.h"

namespace tournament_arena {

struct CandidateLimits {
  std::size_t max_files = 32;
  std::size_t max_file_bytes = 512 * 1024;
  std::size_t max_build_error_bytes = 8 * 1024;  // trimmed, not rejected
};

class CandidateStore : public CandidateView {
 public:
  explicit CandidateStore(std::filesystem::path dir,
                          CandidateLimits limits = {},
                          proto::SubmissionPolicy policy = {});

  void Load();

  // The gate every write passes. Only the sandbox makes a patch safe to build.
  bool Validate(const proto::SubmitRequest &request, std::string *error) const;

  // Id = participant. A READY entry stays until its staged resubmit builds.
  std::optional<proto::Candidate> Create(const proto::SubmitRequest &request,
                                         std::string *error);

  std::optional<proto::Candidate> Get(
      const std::string &candidate_id) const override;

  // |path| is matched against the manifest, so it cannot escape the directory.
  std::optional<std::string> ReadSource(const std::string &candidate_id,
                                        const std::string &path,
                                        std::string *error) const;

  // Newest first.
  std::vector<proto::Candidate> List() const override;

  // For a staged resubmit: READY promotes it, anything else drops it.
  bool SetStatus(const std::string &candidate_id,
                 proto::Candidate::Status status,
                 const std::string &build_error);

  std::size_t size() const;

 private:
  std::filesystem::path CandidateDir(const std::string &candidate_id) const;
  std::filesystem::path StagedDir(const std::string &candidate_id) const;
  bool WriteManifestLocked(const std::filesystem::path &root,
                           const proto::Candidate &candidate) const;

  // The request's own patch, or an add-only one with a generated BUILD.
  std::optional<std::string> PatchForLocked(const proto::SubmitRequest &request,
                                            const std::string &candidate_id,
                                            std::string *error) const;

  const std::filesystem::path dir_;
  const CandidateLimits limits_;
  const proto::SubmissionPolicy policy_;

  mutable std::mutex mutex_;
  std::map<std::string, proto::Candidate> candidates_;
  // Resubmits that have not built yet, beside the entries they would replace.
  std::map<std::string, proto::Candidate> staged_;
};

bool ValidateSourcePath(const std::string &path, std::string *error);

std::string Slugify(const std::string &display_name);

}  // namespace tournament_arena

#endif  // GAME_ARENA_GAME_ARENA_SERVER_CANDIDATE_STORE_H
