#include "game_arena/server/problem_config.h"

#include <google/protobuf/io/tokenizer.h>
#include <google/protobuf/text_format.h>

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <ios>
#include <iterator>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "absl/strings/str_cat.h"
#include "absl/strings/str_replace.h"
#include "absl/strings/string_view.h"
#include "game_arena/common/kv_options/kv_options.h"

namespace tournament_arena {

namespace {

class CollectingErrors final : public google::protobuf::io::ErrorCollector {
 public:
  void RecordError(int line, google::protobuf::io::ColumnNumber column,
                   absl::string_view message) override {
    // Tokenizer positions are zero-based; editors are not.
    absl::StrAppend(&text_, text_.empty() ? "" : "\n", "line ", line + 1, ":",
                    column + 1, ": ", message);
  }

  const std::string& text() const { return text_; }

 private:
  std::string text_;
};

}  // namespace

bool IsValidProblemId(std::string_view problem_id) {
  constexpr std::string_view kLowerAlnum =
      "abcdefghijklmnopqrstuvwxyz0123456789";
  return !problem_id.empty() && problem_id.size() <= 64 &&
         kLowerAlnum.contains(problem_id.front()) &&
         std::ranges::all_of(problem_id, [&](char c) {
           return kLowerAlnum.contains(c) || c == '-' || c == '_';
         });
}

std::string ExpandSubmissionId(std::string_view text,
                               std::string_view submission_id) {
  return absl::StrReplaceAll(text, {{"{submission_id}", submission_id}});
}

std::optional<proto::ProblemConfig> ParseProblemConfigText(
    std::string_view text, std::string* error) {
  proto::ProblemConfig config;
  CollectingErrors errors;
  google::protobuf::TextFormat::Parser parser;
  parser.RecordErrorsTo(&errors);
  // Ignoring a typo'd field would apply a default the author meant to override.
  parser.AllowUnknownField(false);
  if (!parser.ParseFromString(std::string(text), &config)) {
    *error =
        errors.text().empty() ? "could not parse text format" : errors.text();
    return std::nullopt;
  }
  return config;
}

bool ValidateProblemConfig(const proto::ProblemConfig& config,
                           std::string* error) {
  if (!IsValidProblemId(config.problem_id())) {
    *error = absl::StrCat(
        "problem_id ",
        config.problem_id().empty()
            ? "is required"
            : absl::StrCat("'", config.problem_id(), "' is not usable"),
        ": 1-64 chars, starting [a-z0-9], continuing [a-z0-9_-]");
    return false;
  }
  if (config.build().targets().empty()) {
    *error = "build.targets must name at least one bazel target";
    return false;
  }
  if (config.sandbox().image().empty()) {
    // Without one a submission would run as the worker's user: no such mode.
    *error =
        "sandbox.image is required: every submission is built and run in a "
        "container";
    return false;
  }

  switch (config.evaluation_case()) {
    case proto::ProblemConfig::kGrade: {
      const proto::GradeSpec& grade = config.grade();
      if (grade.argv().empty()) {
        *error = "grade.argv is required: nothing to run";
        return false;
      }
      if (grade.metrics().empty()) {
        *error = "grade.metrics must declare at least one metric";
        return false;
      }
      const auto primaries =
          std::ranges::count_if(grade.metrics(), &proto::MetricSpec::primary);
      if (primaries != 1) {
        *error = absl::StrCat(
            "exactly one grade.metrics entry must set primary: true, found ",
            primaries);
        return false;
      }
      break;
    }
    case proto::ProblemConfig::kMatch: {
      const proto::MatchSpec& match = config.match();
      if (match.game().empty()) {
        *error = "match.game is required: it selects the referee's rules";
        return false;
      }
      if (match.referee_target().empty()) {
        *error = "match.referee_target is required";
        return false;
      }
      // They ride to the referee as one "k=v,k2=v2" flag.
      for (const auto& [key, value] : match.registry_options()) {
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
      if (match.players() > 2) {
        if (config.ranking().kind() != proto::RankingSpec::TRUESKILL) {
          *error =
              "match.players above 2 needs ranking.kind TRUESKILL: ELO rates "
              "pairs";
          return false;
        }
        // Each side has each seat as often only over whole rotations.
        if (match.games_per_order() % match.players() != 0 ||
            (match.has_matchmaking() &&
             match.matchmaking().games() % match.players() != 0)) {
          *error = absl::StrCat(
              "match.games_per_order and match.matchmaking.games must be "
              "multiples of match.players (",
              match.players(), ")");
          return false;
        }
        if (static_cast<uint32_t>(match.placement_opponents_size()) + 1 <
            match.players()) {
          *error = absl::StrCat(
              "match.placement_opponents needs at least players - 1 (",
              match.players() - 1, ") builtins to fill a placement game");
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

  if ((config.ranking().kind() == proto::RankingSpec::ELO ||
       config.ranking().kind() == proto::RankingSpec::TRUESKILL) &&
      !config.has_match()) {
    *error = absl::StrCat(
        "ranking.kind ", proto::RankingSpec::Kind_Name(config.ranking().kind()),
        " requires a match evaluation");
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
  const proto::SubmissionPolicy& submission = config.submission();
  if (submission.versions()) {
    // Two versions of one participant can share an order, so a version's
    // patch must stay in its own directory.
    const std::string own =
        submission.files_submit_dir() + "/{submission_id}/**";
    if (submission.files_submit_dir().empty() ||
        submission.allow_paths_size() != 1 ||
        submission.allow_paths(0) != own) {
      *error = absl::StrCat(
          "submission.versions needs files_submit_dir and allow_paths exactly "
          "\"",
          own, "\": two versions of one participant share a tree");
      return false;
    }
  }
  if (config.match().has_matchmaking() &&
      (!submission.versions() ||
       config.ranking().kind() != proto::RankingSpec::TRUESKILL)) {
    *error =
        "match.matchmaking needs submission.versions and ranking.kind "
        "TRUESKILL";
    return false;
  }
  return true;
}

const proto::MetricSpec* PrimaryMetric(const proto::ProblemConfig& config) {
  if (config.ranking().kind() != proto::RankingSpec::METRIC ||
      !config.has_grade()) {
    return nullptr;
  }
  const std::string& named = config.ranking().metric_name();
  for (const proto::MetricSpec& metric : config.grade().metrics()) {
    if (named.empty() ? metric.primary() : metric.name() == named) {
      return &metric;
    }
  }
  return nullptr;
}

std::optional<proto::ProblemConfig> LoadProblemConfig(
    const std::filesystem::path& path, std::string* error) {
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
  if (!ValidateProblemConfig(*config, error)) {
    *error = absl::StrCat(path.string(), ": ", *error);
    return std::nullopt;
  }
  return config;
}

SchedulerConfig SchedulerConfigFor(const proto::ProblemConfig& problem) {
  SchedulerConfig config;
  *config.mutable_build_targets() = problem.build().targets();
  proto::WorkOrder* order = config.mutable_order();
  order->set_build_timeout_s(static_cast<int>(problem.build().timeout_s()));
  *order->mutable_bazel_flags() = problem.build().bazel_flags();

  // The sandbox, translated rather than embedded: see SandboxOrder.
  const auto& sandbox = problem.sandbox();
  auto* order_sandbox = order->mutable_sandbox();
  order_sandbox->set_image(sandbox.image());
  order_sandbox->set_memory_limit_mb(sandbox.memory_limit_mb());
  order_sandbox->set_cpus(sandbox.cpus());
  order_sandbox->set_pids_limit(sandbox.pids_limit());
  order_sandbox->set_build_memory_limit_mb(sandbox.build_memory_limit_mb());
  order_sandbox->set_build_cpus(sandbox.build_cpus());
  order_sandbox->set_run_as_user(sandbox.run_as_user());
  order_sandbox->set_allow_build_network(sandbox.allow_build_network());
  if (problem.has_match()) {
    const auto& match = problem.match();
    *config.mutable_placement_opponents() = match.placement_opponents();
    config.set_players(static_cast<int>(match.players()));
    config.set_placement_games(static_cast<int>(match.games_per_order()));
    order->set_run_timeout_s(static_cast<int>(match.timeout_s()));
    order->set_referee_target(match.referee_target());
    order->set_turn_timeout_ms(match.turn_timeout_ms());
    order->set_game_time_budget_ms(match.game_time_budget_ms());
    order->set_max_moves_per_game(match.max_moves_per_game());
    *order->mutable_registry_options() = match.registry_options();
    // Under the run timeout, so a stuck match comes back as a partial tally.
    order->set_match_deadline_s(std::max(1, order->run_timeout_s() - 30));
    const auto& targets = config.build_targets();
    const auto bot = std::find_if(
        targets.begin(), targets.end(), [](const std::string& target) {
          return target.find("{submission_id}") != std::string::npos;
        });
    if (!targets.empty()) {
      config.set_bot_target(bot != targets.end() ? *bot : targets[0]);
    }
  } else {
    const auto& grade = problem.grade();
    config.set_placement_games(static_cast<int>(grade.repeats()));
    order->set_run_timeout_s(static_cast<int>(grade.timeout_s()));

    auto* graded = order->mutable_grade();
    *graded->mutable_argv() = grade.argv();  // "{submission_id}" still in it
    graded->set_repeats(static_cast<int>(grade.repeats()));
    graded->set_aggregate(static_cast<proto::GradeOrder::Aggregate>(
        static_cast<int>(grade.aggregate())));
    for (const auto& metric : grade.metrics()) {
      graded->add_metric_names(metric.name());
    }
    graded->set_timeout_s(static_cast<int>(grade.timeout_s()));
    graded->set_require_machine_class(grade.require_machine_class());
  }
  return config;
}

}  // namespace tournament_arena
