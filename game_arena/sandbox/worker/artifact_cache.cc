#include "game_arena/sandbox/worker/artifact_cache.h"

#include <grpcpp/grpcpp.h>
#include <unistd.h>

#include <algorithm>
#include <fstream>
#include <utility>

#include "game_arena/common/sha256/sha256.h"

namespace tournament_arena {

namespace {

constexpr std::size_t kChunkBytes = 1 << 20;

}  // namespace

ArtifactCache::ArtifactCache(std::filesystem::path dir,
                             proto::SandboxFleet::StubInterface *fleet)
    : dir_(std::move(dir)), fleet_(fleet) {
  std::filesystem::create_directories(dir_);
}

std::optional<std::filesystem::path> ArtifactCache::Fetch(
    const std::string &digest, std::string *error) {
  // A digest is a file name here, so it is checked like one.
  if (digest.size() != 64 || !std::ranges::all_of(digest, [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
      })) {
    *error = "not a digest: " + digest;
    return std::nullopt;
  }
  const std::filesystem::path path = dir_ / digest;
  std::lock_guard lock(mutex_);
  if (std::filesystem::exists(path)) {
    return path;
  }

  grpc::ClientContext context;
  proto::GetArtifactRequest request;
  request.set_digest(digest);
  const auto reader = fleet_->GetArtifact(&context, request);
  std::string bytes;
  proto::ArtifactChunk chunk;
  while (reader->Read(&chunk)) {
    bytes += chunk.data();
  }
  const grpc::Status status = reader->Finish();
  if (!status.ok()) {
    *error = "cannot fetch " + digest + ": " + status.error_message();
    return std::nullopt;
  }
  if (sha256::Hex(bytes) != digest) {
    *error = "fetched bytes are not " + digest;
    return std::nullopt;
  }
  const std::filesystem::path partial =
      dir_ / (digest + ".partial." + std::to_string(::getpid()));
  std::ofstream(partial, std::ios::binary) << bytes;
  std::filesystem::permissions(partial,
                               std::filesystem::perms::owner_read |
                                   std::filesystem::perms::owner_exec |
                                   std::filesystem::perms::group_read |
                                   std::filesystem::perms::group_exec |
                                   std::filesystem::perms::others_read |
                                   std::filesystem::perms::others_exec);
  std::filesystem::rename(partial, path);
  return path;
}

std::optional<std::string> ArtifactCache::Put(const std::string &bytes,
                                              std::string *error) {
  const std::string digest = sha256::Hex(bytes);
  grpc::ClientContext context;
  proto::PutArtifactResponse response;
  const auto writer = fleet_->PutArtifact(&context, &response);
  proto::ArtifactChunk chunk;
  chunk.set_digest(digest);
  for (std::size_t at = 0; at < bytes.size() || at == 0; at += kChunkBytes) {
    chunk.set_data(bytes.substr(at, kChunkBytes));
    if (!writer->Write(chunk)) {
      break;
    }
  }
  writer->WritesDone();
  const grpc::Status status = writer->Finish();
  if (!status.ok()) {
    *error = "cannot archive " + digest + ": " + status.error_message();
    return std::nullopt;
  }
  return digest;
}

}  // namespace tournament_arena
