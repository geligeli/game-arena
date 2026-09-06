#ifndef GAME_MCTS_GAME_MCTS_TOURNAMENT_SERVER_SERVER_CLIENT_REGISTRY_H
#define GAME_MCTS_GAME_MCTS_TOURNAMENT_SERVER_SERVER_CLIENT_REGISTRY_H

// Who may submit: admin-provisioned tokens, and the quota each one gets.
//
// The registry holds hashes, not tokens. A leaked registry file is therefore
// not a set of usable credentials, and the server never has a raw token to
// leak in the first place -- it hashes what arrives and compares.
//
// Reloadable, because the alternative is that adding one client means a restart
// that drops every attached worker mid-order.
//
// This is an access gate, not a full auth system. It is a bearer token over
// whatever transport the operator configured: if that transport is insecure
// gRPC, the token is visible to anyone on the path. Terminating TLS in front of
// the server is the operator's job, and worth saying out loud rather than
// implying by omission.

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

#include "game_mcts/tournament_server/proto/clients.pb.h"
#include "game_mcts/tournament_server/proto/problem.pb.h"

namespace tournament_arena {

// Lowercase hex SHA-256 of |token|. Exposed so the admin tool and the registry
// agree on the encoding by construction.
auto HashToken(std::string_view token) -> std::string;

// A resolved caller.
struct ClientIdentity {
  std::string client_id;
  std::string display_name;
  proto::ClientQuota quota;
};

class ClientRegistry {
 public:
  // |defaults| fills any quota field a client leaves unset.
  ClientRegistry(std::filesystem::path path, proto::ClientQuota defaults);

  // Reads the file. Returns false with *error set; on failure the previously
  // loaded set is kept, so a typo during a reload does not lock everyone out.
  auto Load(std::string *error) -> bool;

  // The client |token| belongs to, or nullopt when it matches nothing or the
  // client is disabled. Constant-time comparison, so a caller cannot learn a
  // valid hash a byte at a time.
  auto Resolve(std::string_view token) const -> std::optional<ClientIdentity>;

  auto size() const -> std::size_t;

 private:
  const std::filesystem::path path_;
  const proto::ClientQuota defaults_;

  mutable std::mutex mutex_;
  proto::ClientRegistry registry_;
};

}  // namespace tournament_arena

#endif  // GAME_MCTS_GAME_MCTS_TOURNAMENT_SERVER_SERVER_CLIENT_REGISTRY_H
