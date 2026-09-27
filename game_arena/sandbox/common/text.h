#ifndef GAME_ARENA_GAME_ARENA_SANDBOX_COMMON_TEXT_H
#define GAME_ARENA_GAME_ARENA_SANDBOX_COMMON_TEXT_H

#include <cstddef>
#include <string>

namespace sandbox_common {

// The tail, marked as truncated: whatever failed says so at the end.
std::string TailOf(const std::string &text, std::size_t max_chars);

}  // namespace sandbox_common

#endif  // GAME_ARENA_GAME_ARENA_SANDBOX_COMMON_TEXT_H
