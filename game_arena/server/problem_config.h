#ifndef GAME_ARENA_GAME_ARENA_SERVER_PROBLEM_CONFIG_H
#define GAME_ARENA_GAME_ARENA_SERVER_PROBLEM_CONFIG_H

// Loading and checking a ProblemConfig (proto/problem.proto).
//
// A misconfigured problem must fail at startup, not on the first submission an
// agent spends real work on. So loading is read -> parse -> default ->
// validate, and the server refuses to start on any error. Parse errors carry
// the line number; validation errors name the field.
//
// Defaults are applied once, here, rather than read as "0 means X" at each use.
// Consumers -- the scheduler, the worker, the leaderboard -- see a complete
// message and never re-derive a default, which is what keeps the coordinator
// and the workers agreeing on limits they enforce independently.

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#include "game_arena/proto/problem.pb.h"

namespace tournament_arena {

// Parses text format. Returns nullopt with *error set to the protobuf parse
// diagnostics, one "line N: ..." per problem found.
auto ParseProblemConfigText(std::string_view text, std::string *error)
    -> std::optional<proto::ProblemConfig>;

// Fills in every unset field that has a sensible default, so no consumer has to
// know what zero means. Idempotent.
void ApplyProblemDefaults(proto::ProblemConfig *config);

// Internal consistency: an id that is safe as a store key, a repo and build to
// point at, exactly one evaluation mode, and a ranking that matches it. Returns
// false with *error naming the offending field.
//
// Expects defaults to have been applied; a bare parse result will trip checks
// on fields ApplyProblemDefaults fills.
auto ValidateProblemConfig(const proto::ProblemConfig &config,
                           std::string *error) -> bool;

// A repo.url that is a relative filesystem path is taken relative to the
// directory holding the config file, so a problem repo can say `url: "."` and
// stay true wherever it is checked out. URLs -- anything with a scheme, or
// scp-style "git@host:path" -- and absolute paths are left alone.
// LoadProblemConfig applies this; exposed so a config built by hand can too.
void ResolveRelativeRepoUrl(proto::ProblemConfig *config,
                            const std::filesystem::path &config_dir);

// Read + parse + default + validate, with repo.url resolved against the
// config's directory. The one entry point main() should use.
auto LoadProblemConfig(const std::filesystem::path &path, std::string *error)
    -> std::optional<proto::ProblemConfig>;

// The metric the leaderboard orders by: ranking.metric_name if set, otherwise
// the grade metric marked primary. Null for a non-METRIC ranking. Points into
// |config|.
auto PrimaryMetric(const proto::ProblemConfig &config)
    -> const proto::MetricSpec *;

// Substitutes "{submission_id}" in |text|. Applied to build.targets,
// grade.argv and match.referee_target before they reach a worker, so a problem
// whose solutions each live in their own directory can name their targets.
auto ExpandSubmissionId(std::string_view text,
                        std::string_view submission_id) -> std::string;

// True when |problem_id| is safe as a standings key and a path component:
// 1-64 chars, starting with [a-z0-9], continuing with [a-z0-9_-].
auto IsValidProblemId(std::string_view problem_id) -> bool;

}  // namespace tournament_arena

#endif  // GAME_ARENA_GAME_ARENA_SERVER_PROBLEM_CONFIG_H
