#ifndef GAME_ARENA_GAME_ARENA_SANDBOX_COMMON_FILES_H
#define GAME_ARENA_GAME_ARENA_SANDBOX_COMMON_FILES_H

#include <filesystem>
#include <string>

namespace sandbox_common {

// Empty when |path| cannot be opened: a missing log reads as no output.
std::string ReadFile(const std::filesystem::path &path);

// Creates parent directories; false with *error on any failure, short writes
// included.
bool WriteFile(const std::filesystem::path &path, const std::string &content,
               std::string *error);

}  // namespace sandbox_common

#endif  // GAME_ARENA_GAME_ARENA_SANDBOX_COMMON_FILES_H
