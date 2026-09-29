#ifndef GAME_ARENA_GAME_ARENA_SANDBOX_WORKER_ARTIFACT_CACHE_H
#define GAME_ARENA_GAME_ARENA_SANDBOX_WORKER_ARTIFACT_CACHE_H

// This worker's copies of the coordinator's archive, by digest under |dir|:
// fetched once however many slots want one, and read-only, as a digest
// names its bytes for good.

#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

#include "game_arena/proto/arena.grpc.pb.h"

namespace tournament_arena {

class ArtifactCache {
 public:
  ArtifactCache(std::filesystem::path dir,
                proto::SandboxFleet::StubInterface *fleet);

  // The local file for |digest|.
  std::optional<std::filesystem::path> Fetch(const std::string &digest,
                                             std::string *error);
  // Uploads |bytes| to the archive; their digest.
  std::optional<std::string> Put(const std::string &bytes, std::string *error);

 private:
  const std::filesystem::path dir_;
  proto::SandboxFleet::StubInterface *fleet_;  // not owned

  std::mutex mutex_;
  std::map<std::string, std::shared_ptr<std::mutex>> fetching_;
};

}  // namespace tournament_arena

#endif  // GAME_ARENA_GAME_ARENA_SANDBOX_WORKER_ARTIFACT_CACHE_H
