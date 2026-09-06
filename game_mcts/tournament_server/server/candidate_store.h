#ifndef GAME_MCTS_GAME_MCTS_TOURNAMENT_SERVER_SERVER_CANDIDATE_STORE_H
#define GAME_MCTS_GAME_MCTS_TOURNAMENT_SERVER_SERVER_CANDIDATE_STORE_H

// Persistent registry of submissions.
//
// On disk, mirroring GameHistory's layout so both are readable without a tool:
//
//   <dir>/<candidate_id>/manifest.pb    the Candidate proto
//   <dir>/<candidate_id>/patch.diff     the submission itself
//   <dir>/<candidate_id>/src/<path>     files the patch adds, extracted
//   <dir>/index.jsonl                   one line per candidate, append-only
//
// A submission *is* a patch. A structured submission (a list of files plus an
// entry header) is converted into an add-only patch here, at submit time, so
// everything downstream -- the scheduler, the wire, both worker backends --
// handles exactly one form and the worker only ever runs `git apply`.
//
// The added files are also extracted beside the patch, as real files, so a
// human or an agent with a shell can read and grep a rival's source without a
// checkout. The manifest stays the index.
//
// Submitted content is attacker-controlled in the sense that matters here: an
// agent generates it. Validate() is the single gate every write goes through.
// What it can and cannot promise is worth being exact about: it bounds size,
// bounds hunk count, and holds touched paths to the problem's policy. It does
// not make a patch safe to build -- a problem that lets submissions touch BUILD
// files has accepted arbitrary code at build time, and only the sandbox
// contains that.

#include <cstddef>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "game_mcts/tournament_server/proto/arena.pb.h"
#include "game_mcts/tournament_server/proto/problem.pb.h"

namespace tournament_arena {

struct CandidateLimits {
  std::size_t max_files = 32;
  std::size_t max_file_bytes = 512 * 1024;
  std::size_t max_total_bytes = 2 * 1024 * 1024;
  // Trimmed rather than rejected: a build log is diagnostic, and a truncated
  // one is far more useful than none.
  std::size_t max_build_error_bytes = 8 * 1024;
};

// What the store needs from the problem to turn a submission into a patch and
// decide whether to accept it. Filled from ProblemConfig; passed rather than
// read from a global so the store stays testable without a config file.
struct SubmissionRules {
  proto::SubmissionPolicy policy;
  // Directory a structured submission's files are placed under, as
  // <files_submit_dir>/<candidate_id>/<path>. Empty rejects the structured
  // form, requiring every submission to arrive as a patch.
  std::string files_submit_dir;
  // Game key the generated BUILD compiles the candidate harness for. Only used
  // by the structured form.
  std::string game;
};

class CandidateStore {
 public:
  explicit CandidateStore(std::filesystem::path dir,
                          CandidateLimits limits = {},
                          SubmissionRules rules = {});

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

  // The stored patch, verbatim. This is what the scheduler puts on the wire.
  auto ReadPatch(const std::string &candidate_id,
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

  // Builds the patch a request will be stored as: the request's own when it
  // sent one, otherwise a synthesized add-only patch under the problem's
  // files_submit_dir, generated BUILD included.
  auto PatchForLocked(const proto::SubmitRequest &request,
                      const std::string &candidate_id,
                      std::string *error) const -> std::optional<std::string>;

  const std::filesystem::path dir_;
  const CandidateLimits limits_;
  const SubmissionRules rules_;

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

#endif  // GAME_MCTS_GAME_MCTS_TOURNAMENT_SERVER_SERVER_CANDIDATE_STORE_H
