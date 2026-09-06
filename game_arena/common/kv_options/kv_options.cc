#include "game_arena/common/kv_options/kv_options.h"

#include <map>
#include <string>
#include <string_view>

namespace kv_options {

auto Parse(std::string_view text) -> std::map<std::string, std::string> {
  std::map<std::string, std::string> out;
  while (!text.empty()) {
    const std::size_t comma = text.find(',');
    const std::string_view entry = text.substr(0, comma);
    const std::size_t eq = entry.find('=');
    if (!entry.empty() && eq != std::string_view::npos && eq != 0) {
      out[std::string(entry.substr(0, eq))] = std::string(entry.substr(eq + 1));
    }
    if (comma == std::string_view::npos) {
      break;
    }
    text.remove_prefix(comma + 1);
  }
  return out;
}

auto Format(const std::map<std::string, std::string> &options) -> std::string {
  std::string out;
  for (const auto &[key, value] : options) {  // std::map iterates sorted
    if (!IsValidKey(key) || !IsValidValue(value)) {
      continue;
    }
    if (!out.empty()) {
      out += ',';
    }
    out += key;
    out += '=';
    out += value;
  }
  return out;
}

auto IsValidKey(std::string_view key) -> bool {
  return !key.empty() && key.find(',') == std::string_view::npos &&
         key.find('=') == std::string_view::npos;
}

auto IsValidValue(std::string_view value) -> bool {
  return value.find(',') == std::string_view::npos &&
         value.find('=') == std::string_view::npos;
}

}  // namespace kv_options
