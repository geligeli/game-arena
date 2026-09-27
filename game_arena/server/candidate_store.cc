#include "game_arena/server/candidate_store.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <fstream>
#include <string_view>

#include "absl/log/log.h"
#include "absl/strings/ascii.h"
#include "absl/time/clock.h"
#include "absl/time/time.h"
#include "game_arena/server/generated_build.h"
#include "game_arena/server/problem_config.h"
#include "game_arena/server/unified_diff.h"

namespace tournament_arena {

namespace {

// Only what the generated BUILD compiles: a submitted BUILD or script runs.
constexpr std::array<std::string_view, 5> kAllowedExtensions = {
    ".h", ".hpp", ".cc", ".cpp", ".inl"};

bool HasAllowedExtension(const std::string &path) {
  const auto dot = path.rfind('.');
  return dot != std::string::npos &&
         std::ranges::contains(kAllowedExtensions,
                               std::string_view(path).substr(dot));
}

}  // namespace

bool ValidateSourcePath(const std::string &path, std::string *error) {
  if (path.empty()) {
    *error = "empty file path";
    return false;
  }
  if (path.size() > 200) {
    *error = "file path is too long: '" + path + "'";
    return false;
  }
  if (path.front() == '/') {
    *error = "file path must be relative, got '" + path + "'";
    return false;
  }
  // On the raw string: lexically_normal() would quietly resolve "a/../../b".
  if (path.find("..") != std::string::npos) {
    *error = "file path must not contain '..': '" + path + "'";
    return false;
  }
  if (path.find('\\') != std::string::npos) {
    *error = "file path must use '/' separators: '" + path + "'";
    return false;
  }
  if (path.find("//") != std::string::npos || path.back() == '/') {
    *error = "malformed file path: '" + path + "'";
    return false;
  }
  if (!std::ranges::all_of(path, [](char c) {
        return absl::ascii_isalnum(c) || c == '.' || c == '_' || c == '-' ||
               c == '/';
      })) {
    *error = "file path has an unsupported character: '" + path + "'";
    return false;
  }
  if (!HasAllowedExtension(path)) {
    *error = "unsupported file type: '" + path +
             "' (allowed: .h .hpp .cc .cpp .inl)";
    return false;
  }
  return true;
}

std::string Slugify(const std::string &display_name) {
  std::string slug;
  for (const char c : display_name) {
    if (absl::ascii_isalnum(c)) {
      slug += absl::ascii_tolower(c);
    } else if (!slug.empty() && !slug.ends_with('-')) {
      slug += '-';
    }
  }
  // No "--" and no leading '-', so at most one trailing '-' to drop.
  slug.resize(std::min<std::size_t>(slug.size(), 40));
  if (slug.ends_with('-')) {
    slug.pop_back();
  }
  return slug.empty() ? "candidate" : slug;
}

CandidateStore::CandidateStore(std::filesystem::path dir,
                               CandidateLimits limits,
                               proto::SubmissionPolicy policy)
    : dir_(std::move(dir)), limits_(limits), policy_(std::move(policy)) {
  std::error_code ec;
  std::filesystem::create_directories(dir_, ec);
  if (ec) {
    LOG(ERROR) << "Cannot create candidate dir " << dir_ << ": "
               << ec.message();
  }
}

void CandidateStore::Load() {
  std::lock_guard lock(mutex_);
  candidates_.clear();
  std::error_code ec;
  staged_.clear();
  for (const auto &entry : std::filesystem::directory_iterator(dir_, ec)) {
    if (!entry.is_directory()) {
      continue;
    }
    // A staged resubmit's job died with the process that queued it.
    if (entry.path().extension() == ".staged") {
      std::error_code removed;
      std::filesystem::remove_all(entry.path(), removed);
      continue;
    }
    const std::filesystem::path manifest_path = entry.path() / "manifest.pb";
    std::ifstream in(manifest_path, std::ios::binary);
    if (!in) {
      continue;
    }
    proto::CandidateManifest manifest;
    if (!manifest.ParseFromIstream(&in)) {
      LOG(ERROR) << "Corrupt candidate manifest " << manifest_path
                 << "; skipping";
      continue;
    }
    candidates_[manifest.candidate().candidate_id()] = manifest.candidate();
  }
  if (ec) {
    LOG(ERROR) << "Cannot list candidate dir " << dir_ << ": " << ec.message();
  }
  LOG(INFO) << "Loaded " << candidates_.size() << " candidates from " << dir_;
}

namespace {

// Deny wins; no allow pattern allows all; {submission_id} is the submitter's.
bool PathAllowed(const proto::SubmissionPolicy &policy, const std::string &id,
                 const std::string &path, std::string *error) {
  for (const std::string &pattern : policy.deny_paths()) {
    if (PathMatchesGlob(path, ExpandSubmissionId(pattern, id))) {
      *error = "path '" + path + "' is excluded by this problem (deny_paths '" +
               pattern + "')";
      return false;
    }
  }
  if (policy.allow_paths().empty()) {
    return true;
  }
  for (const std::string &pattern : policy.allow_paths()) {
    if (PathMatchesGlob(path, ExpandSubmissionId(pattern, id))) {
      return true;
    }
  }
  *error = "path '" + path +
           "' is outside what this problem accepts; see the problem's "
           "allow_paths";
  return false;
}

}  // namespace

static std::string CandidateIdFor(const proto::SubmitRequest &request) {
  return Slugify(request.author().empty() ? request.display_name()
                                          : request.author());
}

bool CandidateStore::Validate(const proto::SubmitRequest &request,
                              std::string *error) const {
  if (request.display_name().empty()) {
    *error = "display_name is required";
    return false;
  }
  if (request.patch().empty() && request.files().empty()) {
    *error = "a submission needs either a patch or at least one source file";
    return false;
  }

  if (request.patch().empty()) {
    if (policy_.files_submit_dir().empty()) {
      *error =
          "this problem takes patches, not file lists: send a unified diff in "
          "the patch field";
      return false;
    }
    if (static_cast<std::size_t>(request.files_size()) > limits_.max_files) {
      *error = "too many files: " + std::to_string(request.files_size()) +
               " (max " + std::to_string(limits_.max_files) + ")";
      return false;
    }
    std::vector<std::string> seen;
    for (const proto::SourceFile &file : request.files()) {
      if (!ValidateSourcePath(file.path(), error)) {
        return false;
      }
      if (std::find(seen.begin(), seen.end(), file.path()) != seen.end()) {
        *error = "duplicate file path: '" + file.path() + "'";
        return false;
      }
      seen.push_back(file.path());
      if (file.content().size() > limits_.max_file_bytes) {
        *error = "file '" + file.path() + "' is too large (" +
                 std::to_string(file.content().size()) + " bytes, max " +
                 std::to_string(limits_.max_file_bytes) + ")";
        return false;
      }
    }
    if (request.entry_header().empty()) {
      *error = "entry_header is required (the header defining MakePolicy)";
      return false;
    }
    if (std::find(seen.begin(), seen.end(), request.entry_header()) ==
        seen.end()) {
      *error = "entry_header '" + request.entry_header() +
               "' is not among the submitted files";
      return false;
    }
    const auto &allowed_prefixes = policy_.allowed_dep_prefixes();
    for (const std::string &dep : request.extra_deps()) {
      const bool allowed =
          std::any_of(allowed_prefixes.begin(), allowed_prefixes.end(),
                      [&](const std::string &prefix) {
                        return !prefix.empty() && dep.rfind(prefix, 0) == 0;
                      });
      if (!allowed) {
        std::string permitted;
        for (const std::string &prefix : allowed_prefixes) {
          permitted += (permitted.empty() ? "" : ", ") + prefix;
        }
        *error = "dependency '" + dep + "' is not allowed (permitted: " +
                 (permitted.empty() ? "none" : permitted) + ")";
        return false;
      }
    }
  }

  // A synthesized patch is checked too: it is what actually gets built.
  const std::string id = CandidateIdFor(request);
  const std::optional<std::string> patch = PatchForLocked(request, id, error);
  if (!patch.has_value()) {
    return false;
  }
  if (policy_.max_patch_bytes() > 0 &&
      patch->size() > policy_.max_patch_bytes()) {
    *error = "patch is too large (" + std::to_string(patch->size()) +
             " bytes, max " + std::to_string(policy_.max_patch_bytes()) + ")";
    return false;
  }

  Patch parsed;
  if (!ParseUnifiedDiff(*patch, &parsed, error)) {
    return false;
  }
  if (policy_.max_files() > 0 && parsed.files.size() > policy_.max_files()) {
    *error =
        "patch touches too many files: " + std::to_string(parsed.files.size()) +
        " (max " + std::to_string(policy_.max_files()) + ")";
    return false;
  }
  if (policy_.max_hunks() > 0 &&
      static_cast<uint32_t>(parsed.total_hunks) > policy_.max_hunks()) {
    *error = "patch has too many hunks: " + std::to_string(parsed.total_hunks) +
             " (max " + std::to_string(policy_.max_hunks()) + ")";
    return false;
  }
  for (const std::string &path : TouchedPaths(parsed)) {
    if (!PathAllowed(policy_, id, path, error)) {
      return false;
    }
  }

  if (!request.parent_id().empty() &&
      !candidates_.contains(request.parent_id())) {
    *error = "parent_id '" + request.parent_id() + "' is not a known candidate";
    return false;
  }
  return true;
}

std::optional<std::string> CandidateStore::PatchForLocked(
    const proto::SubmitRequest &request, const std::string &candidate_id,
    std::string *error) const {
  if (!request.patch().empty()) {
    return std::string(request.patch());
  }

  const std::string root = policy_.files_submit_dir() + "/" + candidate_id;
  std::vector<NewFile> files;
  std::vector<std::string> paths;
  for (const proto::SourceFile &file : request.files()) {
    files.push_back({root + "/" + file.path(), file.content()});
    paths.push_back(file.path());
  }

  // Generated, never submitted: a submitter's own BUILD could hold a genrule.
  const std::string build = GenerateCandidateBuild(
      policy_.harness(), paths, request.entry_header(),
      {request.extra_deps().begin(), request.extra_deps().end()});
  if (build.empty()) {
    *error =
        "cannot generate a BUILD file for this submission (an entry header "
        "that is not one of the submitted files, or a problem with no "
        "submission.harness configured)";
    return std::nullopt;
  }
  files.push_back({root + "/BUILD", build});
  return MakeAddOnlyPatch(files);
}

std::filesystem::path CandidateStore::CandidateDir(
    const std::string &candidate_id) const {
  return dir_ / candidate_id;
}

// A slug has no '.', so this is never another candidate's directory.
std::filesystem::path CandidateStore::StagedDir(
    const std::string &candidate_id) const {
  return dir_ / (candidate_id + ".staged");
}

bool CandidateStore::WriteManifestLocked(
    const std::filesystem::path &root,
    const proto::Candidate &candidate) const {
  proto::CandidateManifest manifest;
  *manifest.mutable_candidate() = candidate;
  const std::filesystem::path path = root / "manifest.pb";
  // Temp file and rename: Load() would skip a manifest a crash half-wrote.
  const std::filesystem::path tmp = path.string() + ".tmp";
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!out || !manifest.SerializeToOstream(&out)) {
      LOG(ERROR) << "Could not write candidate manifest " << path;
      return false;
    }
  }
  std::error_code ec;
  std::filesystem::rename(tmp, path, ec);
  if (ec) {
    LOG(ERROR) << "Could not install manifest " << path << ": " << ec.message();
    return false;
  }
  return true;
}

std::optional<proto::Candidate> CandidateStore::Create(
    const proto::SubmitRequest &request, std::string *error) {
  std::lock_guard lock(mutex_);
  if (!Validate(request, error)) {
    return std::nullopt;
  }

  proto::Candidate candidate;
  candidate.set_candidate_id(CandidateIdFor(request));
  candidate.set_display_name(request.display_name());
  candidate.set_author(request.author());
  candidate.set_game(request.game());
  candidate.set_parent_id(request.parent_id());
  candidate.set_notes(request.notes());
  candidate.set_entry_header(request.entry_header());
  *candidate.mutable_extra_deps() = request.extra_deps();
  *candidate.mutable_params() = request.params();
  candidate.set_status(proto::Candidate::PENDING);
  candidate.set_submitted_unix_ms(absl::ToUnixMillis(absl::Now()));

  const std::optional<std::string> patch =
      PatchForLocked(request, candidate.candidate_id(), error);
  if (!patch.has_value()) {
    return std::nullopt;
  }
  candidate.set_patch(*patch);

  Patch parsed;
  if (!ParseUnifiedDiff(*patch, &parsed, error)) {
    return std::nullopt;
  }
  for (const std::string &path : TouchedPaths(parsed)) {
    candidate.add_touched_paths(path);
  }

  // A working entry stays the entry until its replacement builds.
  const auto live = candidates_.find(candidate.candidate_id());
  const bool staged = live != candidates_.end() &&
                      live->second.status() == proto::Candidate::READY;
  const std::filesystem::path root =
      staged ? StagedDir(candidate.candidate_id())
             : CandidateDir(candidate.candidate_id());
  std::error_code ec;
  std::filesystem::remove_all(root, ec);
  std::filesystem::create_directories(root / "src", ec);
  if (ec) {
    *error = "cannot create candidate directory: " + ec.message();
    return std::nullopt;
  }

  {
    std::ofstream out(root / "patch.diff", std::ios::binary | std::ios::trunc);
    if (!out) {
      *error = "cannot write patch.diff";
      return std::nullopt;
    }
    out.write(patch->data(), static_cast<std::streamsize>(patch->size()));
    if (!out) {
      *error = "short write for patch.diff";
      return std::nullopt;
    }
  }

  // Added files are also written out plainly, for GetSource and grep.
  for (const PatchFile &file : parsed.files) {
    if (!file.is_new || file.added_content.empty()) {
      continue;
    }
    const std::filesystem::path path = root / "src" / file.path();
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) {
      *error =
          "cannot create directory for '" + file.path() + "': " + ec.message();
      return std::nullopt;
    }
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
      *error = "cannot write '" + file.path() + "'";
      return std::nullopt;
    }
    out.write(file.added_content.data(),
              static_cast<std::streamsize>(file.added_content.size()));
    if (!out) {
      *error = "short write for '" + file.path() + "'";
      return std::nullopt;
    }
    candidate.add_file_paths(file.path());
  }

  if (!WriteManifestLocked(root, candidate)) {
    *error = "cannot write manifest";
    return std::nullopt;
  }
  (staged ? staged_ : candidates_)[candidate.candidate_id()] = candidate;
  LOG(INFO) << "Candidate " << candidate.candidate_id() << " submitted by '"
            << candidate.author() << "' (" << patch->size() << " byte patch, "
            << candidate.touched_paths_size() << " path(s) touched)";
  return candidate;
}

std::optional<proto::Candidate> CandidateStore::Get(
    const std::string &candidate_id) const {
  std::lock_guard lock(mutex_);
  const auto it = candidates_.find(candidate_id);
  if (it == candidates_.end()) {
    return std::nullopt;
  }
  return it->second;
}

std::optional<std::string> CandidateStore::ReadSource(
    const std::string &candidate_id, const std::string &path,
    std::string *error) const {
  proto::Candidate candidate;
  {
    std::lock_guard lock(mutex_);
    const auto it = candidates_.find(candidate_id);
    if (it == candidates_.end()) {
      *error = "unknown candidate '" + candidate_id + "'";
      return std::nullopt;
    }
    candidate = it->second;
  }
  const auto &paths = candidate.file_paths();
  if (std::find(paths.begin(), paths.end(), path) == paths.end()) {
    *error = "candidate '" + candidate_id + "' has no file '" + path + "'";
    return std::nullopt;
  }
  std::ifstream in(CandidateDir(candidate_id) / "src" / path, std::ios::binary);
  if (!in) {
    *error = "cannot read '" + path + "'";
    return std::nullopt;
  }
  return std::string(std::istreambuf_iterator<char>(in),
                     std::istreambuf_iterator<char>());
}

std::vector<proto::Candidate> CandidateStore::List() const {
  std::lock_guard lock(mutex_);
  std::vector<proto::Candidate> out;
  out.reserve(candidates_.size());
  for (const auto &[id, candidate] : candidates_) {
    out.push_back(candidate);
  }
  std::sort(out.begin(), out.end(),
            [](const proto::Candidate &a, const proto::Candidate &b) {
              return a.submitted_unix_ms() > b.submitted_unix_ms();
            });
  return out;
}

bool CandidateStore::SetStatus(const std::string &candidate_id,
                               proto::Candidate::Status status,
                               const std::string &build_error) {
  std::lock_guard lock(mutex_);
  if (const auto staged = staged_.find(candidate_id); staged != staged_.end()) {
    std::error_code ec;
    if (status == proto::Candidate::READY) {
      std::filesystem::remove_all(CandidateDir(candidate_id), ec);
      std::filesystem::rename(StagedDir(candidate_id),
                              CandidateDir(candidate_id), ec);
      candidates_[candidate_id] = staged->second;
    } else {
      std::filesystem::remove_all(StagedDir(candidate_id), ec);
    }
    staged_.erase(staged);
    if (status != proto::Candidate::READY) {
      return true;
    }
  }
  const auto it = candidates_.find(candidate_id);
  if (it == candidates_.end()) {
    return false;
  }
  it->second.set_status(status);
  it->second.set_build_error(
      build_error.size() > limits_.max_build_error_bytes
          ? build_error.substr(build_error.size() -
                               limits_.max_build_error_bytes)
          : build_error);
  return WriteManifestLocked(CandidateDir(candidate_id), it->second);
}

std::size_t CandidateStore::size() const {
  std::lock_guard lock(mutex_);
  return candidates_.size();
}

}  // namespace tournament_arena
