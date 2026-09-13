#include "game_arena/sandbox/common/files.h"

#include <fstream>
#include <iterator>
#include <system_error>

namespace sandbox_common {

std::string ReadFile(const std::filesystem::path &path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return {};
  }
  return std::string(std::istreambuf_iterator<char>(in),
                     std::istreambuf_iterator<char>());
}

bool WriteFile(const std::filesystem::path &path, const std::string &content,
               std::string *error) {
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
