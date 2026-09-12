#include "game_arena/sandbox/common/text.h"

#include <string>

namespace sandbox_common {

auto TailOf(const std::string &text, std::size_t max_chars) -> std::string {
  if (text.size() <= max_chars) {
    return text;
  }
  return "... (" + std::to_string(text.size() - max_chars) +
         " chars truncated) ...\n" + text.substr(text.size() - max_chars);
}

}  // namespace sandbox_common
