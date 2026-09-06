#ifndef GAME_MCTS_GAME_MCTS_TOURNAMENT_SERVER_SANDBOX_WORKER_BOT_LAUNCH_H
#define GAME_MCTS_GAME_MCTS_TOURNAMENT_SERVER_SANDBOX_WORKER_BOT_LAUNCH_H

// Turning an order's bazel labels and params into something to exec.
//
// Shared by both backends so they cannot drift into passing subtly different
// flags for the same order -- a difference that would show up as two hosts
// disagreeing about a candidate's rating, which is the hardest kind of bug to
// see.

#include <google/protobuf/map.h>

#include <string>
#include <string_view>
#include <vector>

namespace tournament_arena {

// The opponent-spec prefix naming another submission, as opposed to
// "builtin:<spec>".
inline constexpr std::string_view kPlayerPrefix = "player:";

// "//a/b:c" -> "a/b/c": where bazel writes a target's binary under bazel-bin.
// "//a/b" is read as "//a/b:b", the same shorthand bazel uses.
auto BinaryPathForTarget(std::string_view target) -> std::string;

// The argv a built bot is started with.
auto BotArgs(const std::string &name, const std::string &target,
             const std::string &opponent, int games,
             const std::string &params) -> std::vector<std::string>;

// "a=1,b=2" from a params map, sorted so a rebuilt candidate gets a
// byte-identical command line and a cached build stays reusable.
auto FormatParams(const google::protobuf::Map<std::string, std::string> &params)
    -> std::string;

}  // namespace tournament_arena

#endif  // GAME_MCTS_GAME_MCTS_TOURNAMENT_SERVER_SANDBOX_WORKER_BOT_LAUNCH_H
