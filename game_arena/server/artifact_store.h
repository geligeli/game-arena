#ifndef GAME_ARENA_GAME_ARENA_SERVER_ARTIFACT_STORE_H
#define GAME_ARENA_GAME_ARENA_SERVER_ARTIFACT_STORE_H

// The archive of built binaries: each at blobs/<sha256> under |dir|, so a
// strategy built once is fetched by every worker that plays it. Named refs
// (refs/<name>) hold a digest: the referee built for an image.

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace tournament_arena {

class ArtifactStore {
 public:
  explicit ArtifactStore(std::filesystem::path dir);

  // Stores |bytes| when their sha256 is |digest|.
  bool Put(std::string_view digest, std::string_view bytes, std::string *error);
  bool Has(std::string_view digest) const;
  std::optional<std::string> Get(std::string_view digest) const;

  // Empty when unset. |name| is [a-z0-9-] only.
  std::string Ref(std::string_view name) const;
  bool SetRef(std::string_view name, std::string_view digest);

 private:
  std::filesystem::path Blob(std::string_view digest) const;

  const std::filesystem::path dir_;
};

// Lowercase hex sha256: the only names a blob may have.
bool IsDigest(std::string_view text);

}  // namespace tournament_arena

#endif  // GAME_ARENA_GAME_ARENA_SERVER_ARTIFACT_STORE_H
