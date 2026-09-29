#ifndef GAME_ARENA_GAME_ARENA_SERVER_CANDIDATE_STORE_H
#define GAME_ARENA_GAME_ARENA_SERVER_CANDIDATE_STORE_H

// Submissions on disk; a file list becomes a patch here, the only form after.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
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

  // Id = participant: a READY entry stays until its staged resubmit builds.
  // With policy.versions, id = <participant>-vNN, a new entry every time.
  std::optional<proto::Candidate> Create(const proto::SubmitRequest &request,
                                         std::string *error);

  // |author|'s newest READY candidate: what their name stands for when every
  // submission is a version of its own.
  std::optional<proto::Candidate> Latest(const std::string &author) const;

  std::optional<proto::Candidate> Get(
      const std::string &candidate_id) const override;

  // |path| is matched against the manifest, so it cannot escape the directory.
  std::optional<std::string> ReadSource(const std::string &candidate_id,
                                        const std::string &path,
                                        std::string *error) const;

  // Newest first.
  std::vector<proto::Candidate> List() const override;

  // An imported submission keeps the time it was first submitted.
  void Backdate(const std::string &candidate_id, int64_t submitted_unix_ms);

  // For a staged resubmit: READY promotes it, anything else drops it.
  bool SetStatus(const std::string &candidate_id,
                 proto::Candidate::Status status,
                 const std::string &build_error);

  std::size_t size() const;
  bool versions() const { return policy_.versions(); }

 private:
  std::filesystem::path CandidateDir(const std::string &candidate_id) const;
  std::filesystem::path StagedDir(const std::string &candidate_id) const;
  bool WriteManifestLocked(const std::filesystem::path &root,
                           const proto::Candidate &candidate) const;

  // The request's own patch, or an add-only one with a generated BUILD.
  std::optional<std::string> PatchForLocked(const proto::SubmitRequest &request,
                                            const std::string &candidate_id,
                                            std::string *error) const;
  // The id |request| would get, and the patch it would store: with versions,
  // the participant's patch moved to the version's directory.
  std::string IdForLocked(const proto::SubmitRequest &request) const;
  std::optional<std::string> FinalPatchLocked(
      const proto::SubmitRequest &request, const std::string &id,
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

// |patch| with <submit_dir>/<from>/ renamed <submit_dir>/<to>/ throughout,
// includes as well as paths: a submission's directory names no one else.
std::string MovePatchDir(std::string_view patch, std::string_view submit_dir,
                         std::string_view from, std::string_view to);

std::string Slugify(const std::string &display_name);

}  // namespace tournament_arena

#endif  // GAME_ARENA_GAME_ARENA_SERVER_CANDIDATE_STORE_H
