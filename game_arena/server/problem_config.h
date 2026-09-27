#ifndef GAME_ARENA_GAME_ARENA_SERVER_PROBLEM_CONFIG_H
#define GAME_ARENA_GAME_ARENA_SERVER_PROBLEM_CONFIG_H

// Defaults are applied once, here, so coordinator and workers agree on limits.

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#include "game_arena/proto/problem.pb.h"

namespace tournament_arena {

// *error gets one "line N: ..." per problem found.
std::optional<proto::ProblemConfig> ParseProblemConfigText(
    std::string_view text, std::string *error);

// Idempotent.
void ApplyProblemDefaults(proto::ProblemConfig *config);

// Expects defaults to have been applied. *error names the offending field.
bool ValidateProblemConfig(const proto::ProblemConfig &config,
                           std::string *error);

std::optional<proto::ProblemConfig> LoadProblemConfig(
    const std::filesystem::path &path, std::string *error);

// ranking.metric_name, else the primary grade metric; null unless METRIC.
const proto::MetricSpec *PrimaryMetric(const proto::ProblemConfig &config);

std::string ExpandSubmissionId(std::string_view text,
                               std::string_view submission_id);

// Safe as a standings key and a path component.
bool IsValidProblemId(std::string_view problem_id);

}  // namespace tournament_arena

#endif  // GAME_ARENA_GAME_ARENA_SERVER_PROBLEM_CONFIG_H
