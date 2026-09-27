#ifndef GAME_ARENA_GAME_ARENA_COMMON_KV_OPTIONS_KV_OPTIONS_H
#define GAME_ARENA_GAME_ARENA_COMMON_KV_OPTIONS_KV_OPTIONS_H

// "k=v,k2=v2" <-> map: one format, written by the worker and read by the
// referee. The problem config is checked with IsValidKey / IsValidValue.

#include <map>
#include <string>
#include <string_view>

namespace kv_options {

// Skips entries with no key or no '='; the last duplicate wins. Never fails:
// a referee that refused to start over one bad setting would fail its order.
std::map<std::string, std::string> Parse(std::string_view text);

// Sorted, so the result is stable. Drops entries that are not valid.
std::string Format(const std::map<std::string, std::string> &options);

bool IsValidKey(std::string_view key);
bool IsValidValue(std::string_view value);

}  // namespace kv_options

#endif  // GAME_ARENA_GAME_ARENA_COMMON_KV_OPTIONS_KV_OPTIONS_H
