#include "game_mcts/tournament_server/sandbox/common/files.h"

#include <fstream>
#include <iterator>
#include <system_error>

namespace sandbox_common {

auto ReadFile(const std::filesystem::path &path) -> std::string {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return {};
  }
  return std::string(std::istreambuf_iterator<char>(in),
                     std::istreambuf_iterator<char>());
}

auto WriteFile(const std::filesystem::path &path, const std::string &content,
               std::string *error) -> bool {
  std::error_code ec;
  if (path.has_parent_path()) {
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) {
      *error =
          "cannot create " + path.parent_path().string() + ": " + ec.message();
      return false;
    }
  }
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) {
    *error = "cannot open " + path.string() + " for writing";
    return false;
  }
  out.write(content.data(), static_cast<std::streamsize>(content.size()));
  if (!out) {
    *error = "short write on " + path.string();
    return false;
  }
  return true;
}

}  // namespace sandbox_common
