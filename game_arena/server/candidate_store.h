#ifndef GAME_ARENA_GAME_ARENA_SERVER_CANDIDATE_STORE_H
#define GAME_ARENA_GAME_ARENA_SERVER_CANDIDATE_STORE_H

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

#include "game_arena/proto/arena.pb.h"
#include "game_arena/proto/problem.pb.h"
#include "game_arena/standings/candidate_view.h"

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
  // What the generated BUILD compiles a structured submission against. Comes
  // from the problem config: the arena does not know what a solution links.
  proto::CandidateHarness harness;
};

class CandidateStore : public CandidateView {
 public:
  explicit CandidateStore(std::filesystem::path dir,
                          CandidateLimits limits = {},
                          SubmissionRules rules = {});

  // Rebuilds the in-memory index from disk. Call once at startup.
  void Load();

  // Validates |request| without storing anything. Returns false with *error
  // set describing the first problem, in terms the submitting agent can act on.
  bool Validate(const proto::SubmitRequest &request, std::string *error) const;

  // Validates, allocates an id, and writes the candidate to disk. Returns
  // nullopt with *error set on a rejected or unwritable submission.
  std::optional<proto::Candidate> Create(const proto::SubmitRequest &request,
                                         std::string *error);

  std::optional<proto::Candidate> Get(
      const std::string &candidate_id) const override;

  // Reads one submitted file. |path| is matched against the manifest's
  // file_paths, so it cannot escape the candidate's directory.
  std::optional<std::string> ReadSource(const std::string &candidate_id,
                                        const std::string &path,
                                        std::string *error) const;

  // The stored patch, verbatim. This is what the scheduler puts on the wire.
  std::optional<std::string> ReadPatch(const std::string &candidate_id,
                                       std::string *error) const;

  // All candidates, newest first.
  std::vector<proto::Candidate> List() const override;

  // Records a build outcome. |build_error| is trimmed to the configured cap.
  bool SetStatus(const std::string &candidate_id,
                 proto::Candidate::Status status,
                 const std::string &build_error);

  std::size_t size() const;

 private:
  std::filesystem::path CandidateDir(const std::string &candidate_id) const;
  // Writes manifest.pb for |candidate|. Caller holds mutex_.
  bool WriteManifestLocked(const proto::Candidate &candidate) const;
  void AppendIndexLocked(const proto::Candidate &candidate) const;
  std::string AllocateIdLocked(const std::string &display_name) const;

  // Builds the patch a request will be stored as: the request's own when it
  // sent one, otherwise a synthesized add-only patch under the problem's
  // files_submit_dir, generated BUILD included.
  std::optional<std::string> PatchForLocked(const proto::SubmitRequest &request,
                                            const std::string &candidate_id,
                                            std::string *error) const;

  const std::filesystem::path dir_;
  const CandidateLimits limits_;
  const SubmissionRules rules_;

  mutable std::mutex mutex_;
  // candidate_id -> manifest. Small (hundreds), and every lookup is on the
  // request path, so it is worth keeping resident.
  std::map<std::string, proto::Candidate> candidates_;
};

// Exposed for testing: the rules a submitted path must satisfy.
bool ValidateSourcePath(const std::string &path, std::string *error);

// Exposed for testing: "My Bot v2!" -> "my-bot-v2".
std::string Slugify(const std::string &display_name);

}  // namespace tournament_arena

#endif  // GAME_ARENA_GAME_ARENA_SERVER_CANDIDATE_STORE_H
