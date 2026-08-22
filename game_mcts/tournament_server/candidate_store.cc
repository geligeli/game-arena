#include "cpp/tournament_server/candidate_store.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <random>
#include <string_view>

#include "absl/log/log.h"

namespace tournament_arena {

namespace {

auto NowUnixMs() -> int64_t {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

// Only sources the generated BUILD knows how to compile. A submission that
// smuggles in a shell script or a BUILD file of its own would otherwise run
// with the worker's privileges at build time.
constexpr std::array<std::string_view, 5> kAllowedExtensions = {
    ".h", ".hpp", ".cc", ".cpp", ".inl"};

// Bazel labels a candidate may name in extra_deps. Anything outside this set
// is refused: the arena is not a general build service, and a dep on an
// arbitrary external repo is a way to run arbitrary code during the build.
constexpr std::array<std::string_view, 4> kAllowedDepPrefixes = {
    "//game_mcts/cpp/mcts:", "//game_mcts/cpp/risk:",
    "//game_mcts/cpp/risk/strategies:", "@abseil-cpp//"};

auto JsonEscape(const std::string &s) -> std::string {
  std::string out;
  out.reserve(s.size());
  for (const char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default: out += c;
    }
  }
  return out;
}

auto HasAllowedExtension(const std::string &path) -> bool {
  const auto dot = path.rfind('.');
  if (dot == std::string::npos) {
    return false;
  }
  const std::string_view ext(path.data() + dot, path.size() - dot);
  return std::find(kAllowedExtensions.begin(), kAllowedExtensions.end(),
                   ext) != kAllowedExtensions.end();
}

}  // namespace

auto ValidateSourcePath(const std::string &path, std::string *error) -> bool {
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
  // Checked on the raw string rather than via std::filesystem, because
  // lexically_normal() would silently resolve "a/../../b" into something that
  // looks fine.
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
  for (const char c : path) {
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                    (c >= '0' && c <= '9') || c == '.' || c == '_' ||
                    c == '-' || c == '/';
    if (!ok) {
      *error = "file path has an unsupported character: '" + path + "'";
      return false;
    }
  }
  if (!HasAllowedExtension(path)) {
    *error = "unsupported file type: '" + path +
             "' (allowed: .h .hpp .cc .cpp .inl)";
    return false;
  }
  return true;
}

auto Slugify(const std::string &display_name) -> std::string {
  std::string slug;
  slug.reserve(display_name.size());
  for (const char c : display_name) {
    if (c >= 'a' && c <= 'z') {
      slug += c;
    } else if (c >= 'A' && c <= 'Z') {
      slug += static_cast<char>(c - 'A' + 'a');
    } else if (c >= '0' && c <= '9') {
      slug += c;
    } else if (!slug.empty() && slug.back() != '-') {
      slug += '-';
    }
  }
  while (!slug.empty() && slug.back() == '-') {
    slug.pop_back();
  }
  if (slug.empty()) {
    slug = "candidate";
  }
  if (slug.size() > 40) {
    slug.resize(40);
    while (!slug.empty() && slug.back() == '-') {
      slug.pop_back();
    }
  }
  return slug;
}

CandidateStore::CandidateStore(std::filesystem::path dir,
                               CandidateLimits limits)
    : dir_(std::move(dir)), limits_(limits) {
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
  for (const auto &entry : std::filesystem::directory_iterator(dir_, ec)) {
    if (!entry.is_directory()) {
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

auto CandidateStore::Validate(const proto::SubmitRequest &request,
                              std::string *error) const -> bool {
  if (request.display_name().empty()) {
    *error = "display_name is required";
    return false;
  }
  if (request.game().empty()) {
    *error = "game is required (e.g. \"risk2\")";
    return false;
  }
  if (request.files().empty()) {
    *error = "at least one source file is required";
    return false;
  }
  if (static_cast<std::size_t>(request.files_size()) > limits_.max_files) {
    *error = "too many files: " + std::to_string(request.files_size()) +
             " (max " + std::to_string(limits_.max_files) + ")";
    return false;
  }

  std::size_t total = 0;
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
    total += file.content().size();
  }
  if (total > limits_.max_total_bytes) {
    *error = "submission is too large (" + std::to_string(total) +
             " bytes, max " + std::to_string(limits_.max_total_bytes) + ")";
    return false;
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

  for (const std::string &dep : request.extra_deps()) {
    const bool allowed =
        std::any_of(kAllowedDepPrefixes.begin(), kAllowedDepPrefixes.end(),
                    [&](std::string_view prefix) {
                      return dep.rfind(prefix, 0) == 0;
                    });
    if (!allowed) {
       *error = "dependency '" + dep +
                "' is not allowed (permitted: //game_mcts/cpp/mcts:, "
                "//game_mcts/cpp/risk:, //game_mcts/cpp/risk/strategies:, @abseil-cpp//)";
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

auto CandidateStore::AllocateIdLocked(const std::string &display_name) const
    -> std::string {
  const std::string slug = Slugify(display_name);
  static thread_local std::mt19937 gen(std::random_device{}());
  std::uniform_int_distribution<int> hex(0, 0xFFFFFF);
  for (int attempt = 0; attempt < 64; ++attempt) {
    char suffix[8];
    std::snprintf(suffix, sizeof(suffix), "%06x", hex(gen));
    std::string id = slug + "-" + suffix;
    if (!candidates_.contains(id)) {
      return id;
    }
  }
  // Astronomically unlikely; fall back to something guaranteed unique rather
  // than returning a colliding id.
  return slug + "-" + std::to_string(NowUnixMs());
}

auto CandidateStore::CandidateDir(const std::string &candidate_id) const
    -> std::filesystem::path {
  return dir_ / candidate_id;
}

auto CandidateStore::WriteManifestLocked(
    const proto::Candidate &candidate) const -> bool {
  proto::CandidateManifest manifest;
  *manifest.mutable_candidate() = candidate;
  const std::filesystem::path path =
      CandidateDir(candidate.candidate_id()) / "manifest.pb";
  // Written via a temp file and renamed, so a crash mid-write cannot leave a
  // half-parsed manifest that Load() would then skip.
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
    LOG(ERROR) << "Could not install manifest " << path << ": "
               << ec.message();
    return false;
  }
  return true;
}

void CandidateStore::AppendIndexLocked(
    const proto::Candidate &candidate) const {
  std::ofstream index(dir_ / "index.jsonl", std::ios::app);
  if (!index) {
    LOG(ERROR) << "Could not append to candidate index in " << dir_;
    return;
  }
  index << "{\"candidate_id\":\"" << JsonEscape(candidate.candidate_id())
        << "\",\"display_name\":\"" << JsonEscape(candidate.display_name())
        << "\",\"author\":\"" << JsonEscape(candidate.author())
        << "\",\"game\":\"" << JsonEscape(candidate.game())
        << "\",\"parent_id\":\"" << JsonEscape(candidate.parent_id())
        << "\",\"base_commit\":\"" << JsonEscape(candidate.base_commit())
        << "\",\"submitted_unix_ms\":" << candidate.submitted_unix_ms()
        << "}\n";
}

auto CandidateStore::Create(const proto::SubmitRequest &request,
                            const std::string &base_commit, std::string *error)
    -> std::optional<proto::Candidate> {
  std::lock_guard lock(mutex_);
  if (!Validate(request, error)) {
    return std::nullopt;
  }

  proto::Candidate candidate;
  candidate.set_candidate_id(AllocateIdLocked(request.display_name()));
  candidate.set_display_name(request.display_name());
  candidate.set_author(request.author());
  candidate.set_game(request.game());
  candidate.set_parent_id(request.parent_id());
  candidate.set_notes(request.notes());
  candidate.set_base_commit(base_commit);
  candidate.set_entry_header(request.entry_header());
  *candidate.mutable_extra_deps() = request.extra_deps();
  *candidate.mutable_params() = request.params();
  candidate.set_status(proto::Candidate::PENDING);
  candidate.set_submitted_unix_ms(NowUnixMs());

  const std::filesystem::path root = CandidateDir(candidate.candidate_id());
  std::error_code ec;
  std::filesystem::create_directories(root / "src", ec);
  if (ec) {
    *error = "cannot create candidate directory: " + ec.message();
    return std::nullopt;
  }

  for (const proto::SourceFile &file : request.files()) {
    const std::filesystem::path path = root / "src" / file.path();
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) {
      *error = "cannot create directory for '" + file.path() +
               "': " + ec.message();
      return std::nullopt;
    }
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
      *error = "cannot write '" + file.path() + "'";
      return std::nullopt;
    }
    out.write(file.content().data(),
              static_cast<std::streamsize>(file.content().size()));
    if (!out) {
      *error = "short write for '" + file.path() + "'";
      return std::nullopt;
    }
    candidate.add_file_paths(file.path());
  }

  if (!WriteManifestLocked(candidate)) {
    *error = "cannot write manifest";
    return std::nullopt;
  }
  AppendIndexLocked(candidate);
  candidates_[candidate.candidate_id()] = candidate;
  LOG(INFO) << "Candidate " << candidate.candidate_id() << " submitted by '"
            << candidate.author() << "' for " << candidate.game() << " ("
            << candidate.file_paths_size() << " files)";
  return candidate;
}

auto CandidateStore::Get(const std::string &candidate_id) const
    -> std::optional<proto::Candidate> {
  std::lock_guard lock(mutex_);
  const auto it = candidates_.find(candidate_id);
  if (it == candidates_.end()) {
    return std::nullopt;
  }
  return it->second;
}

auto CandidateStore::ReadSource(const std::string &candidate_id,
                                const std::string &path,
                                std::string *error) const
    -> std::optional<std::string> {
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
  // Matched against the manifest rather than re-validated: only paths that
  // were accepted at submit time can be read back, so there is no second
  // parser to keep in agreement with the first.
  const auto &paths = candidate.file_paths();
  if (std::find(paths.begin(), paths.end(), path) == paths.end()) {
    *error = "candidate '" + candidate_id + "' has no file '" + path + "'";
    return std::nullopt;
  }
  std::ifstream in(CandidateDir(candidate_id) / "src" / path,
                   std::ios::binary);
  if (!in) {
    *error = "cannot read '" + path + "'";
    return std::nullopt;
  }
  return std::string(std::istreambuf_iterator<char>(in),
                     std::istreambuf_iterator<char>());
}

auto CandidateStore::List() const -> std::vector<proto::Candidate> {
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

auto CandidateStore::SetStatus(const std::string &candidate_id,
                               proto::Candidate::Status status,
                               const std::string &build_error) -> bool {
  std::lock_guard lock(mutex_);
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
  return WriteManifestLocked(it->second);
}

auto CandidateStore::size() const -> std::size_t {
  std::lock_guard lock(mutex_);
  return candidates_.size();
}

}  // namespace tournament_arena
