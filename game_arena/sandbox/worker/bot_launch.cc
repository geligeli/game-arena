#include "game_arena/sandbox/worker/bot_launch.h"

#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace tournament_arena {

std::string BinaryPathForTarget(std::string_view target) {
  std::string label(target);
  if (label.rfind("//", 0) == 0) {
    label = label.substr(2);
  }
  const auto colon = label.rfind(':');
  if (colon != std::string::npos) {
    return label.substr(0, colon) + "/" + label.substr(colon + 1);
  }
  const auto slash = label.rfind('/');
  return slash == std::string::npos ? label + "/" + label
                                    : label + "/" + label.substr(slash + 1);
}

std::vector<std::string> BotArgs(const std::string &name,
                                 const std::string &target,
                                 const std::string &opponent, int games,
                                 const std::string &params) {
  std::vector<std::string> args = {
      "--name=" + name,
      "--server=" + target,
      "--opponent=" + opponent,
      "--games=" + std::to_string(games),
  };
  if (!params.empty()) {
    args.push_back("--params=" + params);
  }
  return args;
}

std::string FormatParams(
    const google::protobuf::Map<std::string, std::string> &params) {
  // proto3 map iteration order is unspecified; sorting keeps a rebuild of the
  // same candidate byte-identical.
  const std::map<std::string, std::string> sorted(params.begin(), params.end());
  std::string out;
  for (const auto &[key, value] : sorted) {
    if (!out.empty()) {
      out += ",";
    }
    out += key + "=" + value;
  }
  return out;
}

}  // namespace tournament_arena
