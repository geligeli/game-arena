#include "game_arena/common/kv_options/kv_options.h"

#include <map>
#include <string>
#include <string_view>

#include "absl/strings/str_cat.h"
#include "absl/strings/str_split.h"

namespace kv_options {

std::map<std::string, std::string> Parse(std::string_view text) {
  std::map<std::string, std::string> out;
  for (std::string_view entry : absl::StrSplit(text, ',')) {
    const std::size_t eq = entry.find('=');
    if (eq != std::string_view::npos && eq != 0) {
      out[std::string(entry.substr(0, eq))] = std::string(entry.substr(eq + 1));
    }
  }
  return out;
}

std::string Format(const std::map<std::string, std::string> &options) {
  std::string out;
  for (const auto &[key, value] : options) {  // std::map iterates sorted
    if (IsValidKey(key) && IsValidValue(value)) {
      absl::StrAppend(&out, out.empty() ? "" : ",", key, "=", value);
    }
  }
  return out;
}

bool IsValidKey(std::string_view key) {
  return !key.empty() && key.find(',') == std::string_view::npos &&
         key.find('=') == std::string_view::npos;
}

bool IsValidValue(std::string_view value) {
  return value.find(',') == std::string_view::npos &&
         value.find('=') == std::string_view::npos;
}

}  // namespace kv_options
