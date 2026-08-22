#include "cpp/tournament_server/candidate/candidate_params.h"

namespace candidate {

namespace {

auto Trim(std::string_view text) -> std::string_view {
  constexpr std::string_view kSpace = " \t\r\n";
  const auto begin = text.find_first_not_of(kSpace);
  if (begin == std::string_view::npos) {
    return {};
  }
  return text.substr(begin, text.find_last_not_of(kSpace) - begin + 1);
}

}  // namespace

auto Params::Parse(std::string_view spec) -> Params {
  std::map<std::string, std::string> values;
  while (!spec.empty()) {
    const auto comma = spec.find(',');
    const std::string_view entry =
        comma == std::string_view::npos ? spec : spec.substr(0, comma);
    spec = comma == std::string_view::npos ? std::string_view{}
                                           : spec.substr(comma + 1);

    const auto equals = entry.find('=');
    if (equals == std::string_view::npos) {
      continue;
    }
    const std::string_view key = Trim(entry.substr(0, equals));
    if (key.empty()) {
      continue;
    }
    values[std::string(key)] = std::string(Trim(entry.substr(equals + 1)));
  }
  return Params(std::move(values));
}

}  // namespace candidate
