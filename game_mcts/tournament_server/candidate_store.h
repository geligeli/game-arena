#ifndef RISK_GAME_AI_CPP_TOURNAMENT_SERVER_CANDIDATE_STORE_H
#define RISK_GAME_AI_CPP_TOURNAMENT_SERVER_CANDIDATE_STORE_H

// Persistent registry of submitted strategies.
//
// On disk, mirroring GameHistory's layout so both are readable without a tool:
//
//   <dir>/<candidate_id>/manifest.pb    the Candidate proto
//   <dir>/<candidate_id>/src/<path>     the submitted sources, verbatim
//   <dir>/index.jsonl                   one line per candidate, append-only
//
// Sources are written out as real files rather than kept inside the manifest
// so a human (or an agent with a shell) can read, grep and diff them directly;
// the manifest stays the index.
//
// Submitted paths are attacker-controlled in the sense that matters here: an
// agent generates them. Validate() is therefore the single gate every write
// goes through, and it is deliberately strict -- an allowlist of extensions
// under a relative path, no symlinks, bounded size and count.

#include <cstddef>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "game_mcts/tournament_server/arena.pb.h"

namespace tournament_arena {

struct CandidateLimits {
  std::size_t max_files = 32;
  std::size_t max_file_bytes = 512 * 1024;
  std::size_t max_total_bytes = 2 * 1024 * 1024;
  // Trimmed rather than rejected: a build log is diagnostic, and a truncated
  // one is far more useful than none.
  std::size_t max_build_error_bytes = 8 * 1024;
};

class CandidateStore {
 public:
  explicit CandidateStore(std::filesystem::path dir,
                          CandidateLimits limits = {});

  // Rebuilds the in-memory index from disk. Call once at startup.
  void Load();

  // Validates |request| without storing anything. Returns false with *error
  // set describing the first problem, in terms the submitting agent can act on.
  auto Validate(const proto::SubmitRequest &request,
                std::string *error) const -> bool;

  // Validates, allocates an id, and writes the candidate to disk. Returns
  // nullopt with *error set on a rejected or unwritable submission.
  auto Create(const proto::SubmitRequest &request,
              const std::string &base_commit,
              std::string *error) -> std::optional<proto::Candidate>;

  auto Get(const std::string &candidate_id) const
      -> std::optional<proto::Candidate>;

  // Reads one submitted file. |path| is matched against the manifest's
  // file_paths, so it cannot escape the candidate's directory.
  auto ReadSource(const std::string &candidate_id, const std::string &path,
                  std::string *error) const -> std::optional<std::string>;

  // All candidates, newest first.
  auto List() const -> std::vector<proto::Candidate>;

  // Records a build outcome. |build_error| is trimmed to the configured cap.
  auto SetStatus(const std::string &candidate_id,
                 proto::Candidate::Status status,
                 const std::string &build_error) -> bool;

  auto size() const -> std::size_t;

 private:
  auto CandidateDir(const std::string &candidate_id) const
      -> std::filesystem::path;
  // Writes manifest.pb for |candidate|. Caller holds mutex_.
  auto WriteManifestLocked(const proto::Candidate &candidate) const -> bool;
  void AppendIndexLocked(const proto::Candidate &candidate) const;
  auto AllocateIdLocked(const std::string &display_name) const -> std::string;

  const std::filesystem::path dir_;
  const CandidateLimits limits_;

  mutable std::mutex mutex_;
  // candidate_id -> manifest. Small (hundreds), and every lookup is on the
  // request path, so it is worth keeping resident.
  std::map<std::string, proto::Candidate> candidates_;
};

// Exposed for testing: the rules a submitted path must satisfy.
auto ValidateSourcePath(const std::string &path, std::string *error) -> bool;

// Exposed for testing: "My Bot v2!" -> "my-bot-v2".
auto Slugify(const std::string &display_name) -> std::string;

}  // namespace tournament_arena

#endif  // RISK_GAME_AI_CPP_TOURNAMENT_SERVER_CANDIDATE_STORE_H
