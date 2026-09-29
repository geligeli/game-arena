#ifndef GAME_ARENA_GAME_ARENA_COMMON_SHA256_SHA256_H
#define GAME_ARENA_GAME_ARENA_COMMON_SHA256_SHA256_H

#include <string>
#include <string_view>

namespace sha256 {

// Lowercase hex.
std::string Hex(std::string_view data);

}  // namespace sha256

#endif  // GAME_ARENA_GAME_ARENA_COMMON_SHA256_SHA256_H
