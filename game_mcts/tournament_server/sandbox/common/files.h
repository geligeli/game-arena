#ifndef GAME_MCTS_GAME_MCTS_TOURNAMENT_SERVER_SANDBOX_COMMON_FILES_H
#define GAME_MCTS_GAME_MCTS_TOURNAMENT_SERVER_SANDBOX_COMMON_FILES_H

// Small file helpers shared by the standalone sandbox runner and the fleet
// worker's backends: reading a captured log back, and staging a file with
// the write checked rather than hoped for.

#include <filesystem>
#include <string>

namespace sandbox_common {

// The file's whole contents, or empty when it cannot be opened. Callers use
// this for logs and reports, where a missing file reads as "no output".
auto ReadFile(const std::filesystem::path &path) -> std::string;

// Writes |content| to |path|, creating parent directories. Returns false with
// *error set when the file cannot be opened or the write comes up short -- a
// staged patch that silently truncated would otherwise fail much later, as a
// confusing build error.
auto WriteFile(const std::filesystem::path &path, const std::string &content,
               std::string *error) -> bool;

}  // namespace sandbox_common

#endif  // GAME_MCTS_GAME_MCTS_TOURNAMENT_SERVER_SANDBOX_COMMON_FILES_H
