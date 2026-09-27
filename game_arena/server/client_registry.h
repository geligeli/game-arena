#ifndef GAME_ARENA_GAME_ARENA_SERVER_CLIENT_REGISTRY_H
#define GAME_ARENA_GAME_ARENA_SERVER_CLIENT_REGISTRY_H

// Who may submit. It holds token hashes, never tokens; TLS is the operator's.

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

#include "game_arena/proto/clients.pb.h"
#include "game_arena/proto/problem.pb.h"

namespace tournament_arena {

// Lowercase hex SHA-256.
std::string HashToken(std::string_view token);

// 256 random bits, lowercase hex.
std::string MintToken();

// Zero quota fields fall back to the problem's defaults at resolve time.
proto::Client MakeClient(std::string_view client_id,
                         std::string_view display_name, std::string_view token,
                         const proto::ClientQuota &quota);

std::string ClientBlockText(const proto::Client &client);

// Appends |client| if absent. The rewrite drops the file's comments.
bool SetClientToken(const std::filesystem::path &path,
                    const proto::Client &client, std::string *error);

// Refuses a registry that does not load or already has this client_id.
bool AppendClientToRegistry(const std::filesystem::path &path,
                            const proto::Client &client, std::string *error);

struct ClientIdentity {
  std::string client_id;
  std::string display_name;
  proto::ClientQuota quota;
};

class ClientRegistry {
 public:
  ClientRegistry(std::filesystem::path path, proto::ClientQuota defaults);

  // On failure the loaded set is kept, so a bad reload locks nobody out.
  bool Load(std::string *error);

  // Nullopt for an unknown or disabled client.
  std::optional<ClientIdentity> Resolve(std::string_view token) const;

  std::size_t size() const;

 private:
  const std::filesystem::path path_;
  const proto::ClientQuota defaults_;

  mutable std::mutex mutex_;
  proto::ClientRegistry registry_;
};

}  // namespace tournament_arena

#endif  // GAME_ARENA_GAME_ARENA_SERVER_CLIENT_REGISTRY_H
