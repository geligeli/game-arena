#include "game_arena/server/problem_config.h"

#include <google/protobuf/io/tokenizer.h>
#include <google/protobuf/text_format.h>

#include <cctype>
#include <cstddef>
#include <fstream>
#include <ios>
#include <iterator>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "game_arena/common/kv_options/kv_options.h"

namespace tournament_arena {

namespace {

// Collects every parse diagnostic instead of only the first, so a config with
// three typos takes one edit round rather than three.
class CollectingErrors final : public google::protobuf::io::ErrorCollector {
 public:
  void RecordError(int line, google::protobuf::io::ColumnNumber column,
                   absl::string_view message) override {
    // Tokenizer positions are zero-based; editors are not.
    absl::StrAppend(&text_, text_.empty() ? "" : "\n", "line ", line + 1, ":",
                    column + 1, ": ", message);
  }

  auto text() const -> const std::string & { return text_; }

 private:
  std::string text_;
};

// "https://...", "ssh://...", "file://..." have a scheme; "git@host:path" is
// scp-style. Everything else is a filesystem path.
auto LooksLikeUrl(std::string_view url) -> bool {
  if (url.find("://") != std::string_view::npos) {
    return true;
  }
  const std::size_t colon = url.find(':');
  const std::size_t slash = url.find('/');
  return colon != std::string_view::npos &&
         (slash == std::string_view::npos || colon < slash);
}

auto CountPrimaryMetrics(const proto::GradeSpec &grade) -> int {
  int primaries = 0;
  for (const proto::MetricSpec &metric : grade.metrics()) {
    primaries += metric.primary() ? 1 : 0;
  }
  return primaries;
}

}  // namespace

auto IsValidProblemId(std::string_view problem_id) -> bool {
  if (problem_id.empty() || problem_id.size() > 64) {
    return false;
  }
  const auto is_lower_alnum = [](char c) {
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
  };
  if (!is_lower_alnum(problem_id.front())) {
    return false;
  }
  for (const char c : problem_id) {
    if (!is_lower_alnum(c) && c != '-' && c != '_') {
      return false;
    }
  }
  return true;
}

auto ExpandSubmissionId(std::string_view text,
                        std::string_view submission_id) -> std::string {
  constexpr std::string_view kPlaceholder = "{submission_id}";
  std::string out;
  out.reserve(text.size());
  for (std::size_t at = 0; at < text.size();) {
    const std::size_t hit = text.find(kPlaceholder, at);
    if (hit == std::string_view::npos) {
      out.append(text.substr(at));
      break;
    }
    out.append(text.substr(at, hit - at));
    out.append(submission_id);
    at = hit + kPlaceholder.size();
  }
  return out;
}

auto ParseProblemConfigText(std::string_view text, std::string *error)
    -> std::optional<proto::ProblemConfig> {
  proto::ProblemConfig config;
  CollectingErrors errors;
  google::protobuf::TextFormat::Parser parser;
  parser.RecordErrorsTo(&errors);
  // An unknown field is a typo or a config written against a newer server. Both
  // are worth failing on: silently ignoring it would apply a default the author
  // believed they had overridden.
  parser.AllowUnknownField(false);
  if (!parser.ParseFromString(std::string(text), &config)) {
    *error =
        errors.text().empty() ? "could not parse text format" : errors.text();
    return std::nullopt;
  }
  return config;
}

void ApplyProblemDefaults(proto::ProblemConfig *config) {
  // Unset and "explicitly zero" are the same thing for a proto3 scalar, which
  // is exactly right here: every field defaulted below is a limit where zero
  // would be nonsense anyway.
  if (config->repo().base_commit().empty()) {
    config->mutable_repo()->set_base_commit("HEAD");
  }

  proto::SubmissionPolicy *submission = config->mutable_submission();
  if (submission->max_patch_bytes() == 0) {
    submission->set_max_patch_bytes(2ULL * 1024 * 1024);
  }
  if (submission->max_files() == 0) {
    submission->set_max_files(64);
  }
  if (submission->max_hunks() == 0) {
    submission->set_max_hunks(512);
  }

  if (config->build().timeout_s() == 0) {
    config->mutable_build()->set_timeout_s(1800);
  }

  proto::SandboxSpec *sandbox = config->mutable_sandbox();
  if (sandbox->memory_limit_mb() == 0) {
    sandbox->set_memory_limit_mb(4096);
  }
  if (sandbox->pids_limit() == 0) {
    sandbox->set_pids_limit(512);
  }
  // Defaults to on: the in-container alternative needs CAP_SYS_ADMIN, and a
  // container with that running submitted build code is not a boundary. The
  // companion bool is what lets an explicit `false` survive defaulting.
  if (!sandbox->host_overlay_set()) {
    sandbox->set_host_overlay(true);
    sandbox->set_host_overlay_set(true);
  }

  if (config->has_grade()) {
    proto::GradeSpec *grade = config->mutable_grade();
    if (grade->repeats() == 0) {
      grade->set_repeats(3);
    }
    if (grade->timeout_s() == 0) {
      grade->set_timeout_s(1800);
    }
  }
  if (config->has_match()) {
    proto::MatchSpec *match = config->mutable_match();
    if (match->games_per_order() == 0) {
      match->set_games_per_order(10);
    }
    if (match->turn_timeout_ms() == 0) {
      match->set_turn_timeout_ms(10000);
    }
    if (match->max_moves_per_game() == 0) {
      match->set_max_moves_per_game(50000);
    }
    if (match->timeout_s() == 0) {
      match->set_timeout_s(1800);
    }
  }

  proto::ClientQuota *quota =
      config->mutable_clients()->mutable_default_quota();
  // No sentinel for "unlimited", deliberately: a quota that can be switched off
  // is a quota nobody notices is off.
  if (quota->max_active_evaluations() == 0) {
    quota->set_max_active_evaluations(1);
  }
  if (quota->max_queued_jobs() == 0) {
    quota->set_max_queued_jobs(8);
  }
}

auto ValidateProblemConfig(const proto::ProblemConfig &config,
                           std::string *error) -> bool {
  if (!IsValidProblemId(config.problem_id())) {
    *error = absl::StrCat(
        "problem_id ",
        config.problem_id().empty()
            ? "is required"
            : absl::StrCat("'", config.problem_id(), "' is not usable"),
        ": 1-64 chars, starting [a-z0-9], continuing [a-z0-9_-]");
    return false;
  }
  if (config.repo().url().empty()) {
    *error = "repo.url is required";
    return false;
  }
  if (config.build().targets().empty()) {
    *error = "build.targets must name at least one bazel target";
    return false;
  }
  if (config.sandbox().image().empty() &&
      config.sandbox().require_container()) {
    *error = "sandbox.image is required when sandbox.require_container is set";
    return false;
  }

  switch (config.evaluation_case()) {
    case proto::ProblemConfig::kGrade: {
      const proto::GradeSpec &grade = config.grade();
      if (grade.argv().empty()) {
        *error = "grade.argv is required: nothing to run";
        return false;
      }
      if (grade.metrics().empty()) {
        *error = "grade.metrics must declare at least one metric";
        return false;
      }
      const int primaries = CountPrimaryMetrics(grade);
      if (primaries != 1) {
        *error = absl::StrCat(
            "exactly one grade.metrics entry must set primary: true, found ",
            primaries);
        return false;
      }
      break;
    }
    case proto::ProblemConfig::kMatch: {
      const proto::MatchSpec &match = config.match();
      if (match.game().empty()) {
        *error = "match.game is required: it selects the referee's rules";
        return false;
      }
      if (match.referee_target().empty()) {
        *error = "match.referee_target is required";
        return false;
      }
      // These ride to the referee as a "k=v,k2=v2" flag, so a key or value
      // carrying a separator would arrive as something else entirely. Caught
      // here, at load, rather than as a puzzling referee on a worker.
      for (const auto &[key, value] : match.registry_options()) {
        if (!kv_options::IsValidKey(key)) {
          *error =
              absl::StrCat("match.registry_options has an invalid key '", key,
                           "': keys must be non-empty and contain "
                           "no ',' or '='");
          return false;
        }
        if (!kv_options::IsValidValue(value)) {
          *error = absl::StrCat("match.registry_options['", key,
                                "'] contains ',' or '=', which the referee's "
                                "option list cannot carry");
          return false;
        }
      }
      break;
    }
    case proto::ProblemConfig::EVALUATION_NOT_SET:
      *error =
          "one of grade or match is required: a problem with no evaluation "
          "cannot rank anything";
      return false;
  }

  // Ranking and evaluation have to agree, or the leaderboard reads a score that
  // is never written.
  if (config.ranking().kind() == proto::RankingSpec::ELO &&
      !config.has_match()) {
    *error = "ranking.kind ELO requires a match evaluation";
    return false;
  }
  if (config.ranking().kind() == proto::RankingSpec::METRIC) {
    if (!config.has_grade()) {
      *error = "ranking.kind METRIC requires a grade evaluation";
      return false;
    }
    if (PrimaryMetric(config) == nullptr) {
      *error =
          absl::StrCat("ranking.metric_name '", config.ranking().metric_name(),
                       "' names no metric in grade.metrics");
      return false;
    }
  }
  return true;
}

auto PrimaryMetric(const proto::ProblemConfig &config)
    -> const proto::MetricSpec * {
  if (config.ranking().kind() != proto::RankingSpec::METRIC ||
      !config.has_grade()) {
    return nullptr;
  }
  const std::string &named = config.ranking().metric_name();
  for (const proto::MetricSpec &metric : config.grade().metrics()) {
    if (named.empty() ? metric.primary() : metric.name() == named) {
      return &metric;
    }
  }
  return nullptr;
}

void ResolveRelativeRepoUrl(proto::ProblemConfig *config,
                            const std::filesystem::path &config_dir) {
  const std::string &url = config->repo().url();
  if (url.empty() || LooksLikeUrl(url)) {
    return;
  }
  const std::filesystem::path path(url);
  if (path.is_absolute()) {
    return;
  }
  // weakly_canonical resolves whatever prefix exists and normalizes the rest,
  // so a "." or ".." comes out clean whether or not the target is there yet.
  std::error_code ec;
  std::filesystem::path resolved =
      std::filesystem::weakly_canonical(config_dir / path, ec);
  if (ec) {
    resolved = (config_dir / path).lexically_normal();
  }
  // "dir/." normalizes to "dir/"; a trailing separator is noise in a URL.
  std::string url_out = resolved.string();
  while (url_out.size() > 1 && url_out.back() == '/') {
    url_out.pop_back();
  }
  config->mutable_repo()->set_url(url_out);
}

auto LoadProblemConfig(const std::filesystem::path &path, std::string *error)
    -> std::optional<proto::ProblemConfig> {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    *error = absl::StrCat("cannot read problem config ", path.string());
    return std::nullopt;
  }
  const std::string text((std::istreambuf_iterator<char>(in)),
                         std::istreambuf_iterator<char>());

  std::optional<proto::ProblemConfig> config =
      ParseProblemConfigText(text, error);
  if (!config) {
    *error = absl::StrCat(path.string(), ": ", *error);
    return std::nullopt;
  }
  std::error_code ec;
  const std::filesystem::path config_dir =
      std::filesystem::absolute(path, ec).parent_path();
  ResolveRelativeRepoUrl(&*config, ec ? path.parent_path() : config_dir);
  ApplyProblemDefaults(&*config);
  if (!ValidateProblemConfig(*config, error)) {
    *error = absl::StrCat(path.string(), ": ", *error);
    return std::nullopt;
  }
  return config;
}

}  // namespace tournament_arena
