#include "game_arena/server/artifact_store.h"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <utility>

#include "absl/strings/ascii.h"
#include "game_arena/common/sha256/sha256.h"
#include "game_arena/standings/game_history.h"

namespace tournament_arena {

namespace {

std::optional<std::string> ReadAll(const std::filesystem::path &path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return std::nullopt;
  }
  return std::string(std::istreambuf_iterator<char>(in), {});
}

bool IsRefName(std::string_view name) {
  return !name.empty() && std::ranges::all_of(name, [](char c) {
    return absl::ascii_islower(c) || absl::ascii_isdigit(c) || c == '-';
  });
}

}  // namespace

bool IsDigest(std::string_view text) {
  return text.size() == 64 && std::ranges::all_of(text, [](char c) {
           return absl::ascii_isdigit(c) || (c >= 'a' && c <= 'f');
         });
}

ArtifactStore::ArtifactStore(std::filesystem::path dir) : dir_(std::move(dir)) {
  std::filesystem::create_directories(dir_ / "blobs");
  std::filesystem::create_directories(dir_ / "refs");
}

std::filesystem::path ArtifactStore::Blob(std::string_view digest) const {
  return dir_ / "blobs" / std::string(digest);
}

bool ArtifactStore::Put(std::string_view digest, std::string_view bytes,
                        std::string *error) {
  if (!IsDigest(digest) || sha256::Hex(bytes) != digest) {
    *error = "the bytes are not " + std::string(digest);
    return false;
  }
  return Has(digest) ||
         tournament_broker::WriteAtomically(Blob(digest), bytes, error);
}

bool ArtifactStore::Has(std::string_view digest) const {
  return IsDigest(digest) && std::filesystem::exists(Blob(digest));
}

std::optional<std::string> ArtifactStore::Get(std::string_view digest) const {
  return IsDigest(digest) ? ReadAll(Blob(digest)) : std::nullopt;
}

std::string ArtifactStore::Ref(std::string_view name) const {
  const auto digest =
      IsRefName(name) ? ReadAll(dir_ / "refs" / std::string(name)) : "";
  return digest.has_value() && Has(*digest) ? *digest : "";
}

bool ArtifactStore::SetRef(std::string_view name, std::string_view digest) {
  std::string error;
  return IsRefName(name) && Has(digest) &&
         tournament_broker::WriteAtomically(dir_ / "refs" / std::string(name),
                                            digest, &error);
}

}  // namespace tournament_arena
