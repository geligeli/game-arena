#ifndef RISK_GAME_AI_CPP_TOURNAMENT_SERVER_CANDIDATE_CANDIDATE_PARAMS_H
#define RISK_GAME_AI_CPP_TOURNAMENT_SERVER_CANDIDATE_CANDIDATE_PARAMS_H

// Tuning knobs the arena passes to a candidate, as "key=value,key=value".
//
// Split out of candidate_api.h and kept free of any game selection on purpose:
// candidate_api.h resolves game_t from the CANDIDATE_GAME_* define, so a
// translation unit compiled without that define must not also define anything
// with linkage. Params has one compiled definition for everyone; the
// game-dependent part stays header-only.

#include <cstdlib>
#include <map>
#include <string>
#include <string_view>
#include <utility>

namespace candidate {

class Params {
 public:
  Params() = default;
  explicit Params(std::map<std::string, std::string> values)
      : values_(std::move(values)) {}

  // Parses "iterations=800, exploration_c=1.4". Whitespace around keys and
  // values is trimmed and empty entries are skipped. An entry without '=' is
  // ignored rather than fatal: a candidate must still start when the arena
  // passes something it does not understand.
  static auto Parse(std::string_view spec) -> Params;

  // Every getter falls back rather than throwing, including on a value that
  // does not parse as the requested type -- a malformed knob must not take a
  // candidate out of the tournament.
  auto get(std::string_view key,
           std::string_view fallback) const -> std::string {
    const auto it = values_.find(std::string(key));
    return it == values_.end() ? std::string(fallback) : it->second;
  }

  auto get_int(std::string_view key, int fallback) const -> int {
    const auto it = values_.find(std::string(key));
    if (it == values_.end()) {
      return fallback;
    }
    char *end = nullptr;
    const long parsed = std::strtol(it->second.c_str(), &end, 10);
    return (end == it->second.c_str() || *end != '\0')
               ? fallback
               : static_cast<int>(parsed);
  }

  auto get_double(std::string_view key, double fallback) const -> double {
    const auto it = values_.find(std::string(key));
    if (it == values_.end()) {
      return fallback;
    }
    char *end = nullptr;
    const double parsed = std::strtod(it->second.c_str(), &end);
    return (end == it->second.c_str() || *end != '\0') ? fallback : parsed;
  }

  auto contains(std::string_view key) const -> bool {
    return values_.find(std::string(key)) != values_.end();
  }

  auto values() const -> const std::map<std::string, std::string> & {
    return values_;
  }

 private:
  std::map<std::string, std::string> values_;
};

}  // namespace candidate

#endif  // RISK_GAME_AI_CPP_TOURNAMENT_SERVER_CANDIDATE_CANDIDATE_PARAMS_H
