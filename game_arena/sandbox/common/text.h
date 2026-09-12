#ifndef GAME_ARENA_GAME_ARENA_SANDBOX_COMMON_TEXT_H
#define GAME_ARENA_GAME_ARENA_SANDBOX_COMMON_TEXT_H

// Trimming captured output down to something worth reporting.
//
// Here rather than beside the build-log compactor it used to live with,
// because every layer needs it and only one of them knows what bazel is: a
// clone that fails, a container that will not start, a step that dies before
// it says why. The failure text of a generic sandbox has to be trimmable
// without the sandbox depending on the arena.

#include <cstddef>
#include <string>

namespace sandbox_common {

// Keeps the last |max_chars| of |text|, marking what was dropped. The tail
// rather than the head: whatever failed says so at the end.
auto TailOf(const std::string &text, std::size_t max_chars) -> std::string;

}  // namespace sandbox_common

#endif  // GAME_ARENA_GAME_ARENA_SANDBOX_COMMON_TEXT_H
