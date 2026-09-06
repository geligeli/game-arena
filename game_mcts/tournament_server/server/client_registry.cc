#include "game_mcts/tournament_server/server/client_registry.h"

#include <google/protobuf/text_format.h>
#include <openssl/sha.h>

#include <cstddef>
#include <fstream>
#include <ios>
#include <iterator>
#include <string>
#include <utility>

#include "absl/log/log.h"
#include "absl/strings/ascii.h"
#include "absl/strings/str_cat.h"

namespace tournament_arena {

namespace {

// Equal-length, data-independent comparison. A byte-at-a-time early return
// would let a caller with a stopwatch learn a valid hash one byte at a time.
auto ConstantTimeEquals(std::string_view a, std::string_view b) -> bool {
  if (a.size() != b.size()) {
    return false;
  }
  unsigned char diff = 0;
  for (std::size_t i = 0; i < a.size(); ++i) {
    diff |= static_cast<unsigned char>(a[i]) ^ static_cast<unsigned char>(b[i]);
  }
  return diff == 0;
}

}  // namespace

auto HashToken(std::string_view token) -> std::string {
  unsigned char digest[SHA256_DIGEST_LENGTH];
  ::SHA256(reinterpret_cast<const unsigned char *>(token.data()), token.size(),
           digest);
  std::string hex;
  hex.reserve(sizeof(digest) * 2);
  static constexpr char kHex[] = "0123456789abcdef";
  for (const unsigned char byte : digest) {
    hex.push_back(kHex[byte >> 4]);
    hex.push_back(kHex[byte & 0x0f]);
  }
  return hex;
}

ClientRegistry::ClientRegistry(std::filesystem::path path,
                               proto::ClientQuota defaults)
    : path_(std::move(path)), defaults_(std::move(defaults)) {}

auto ClientRegistry::Load(std::string *error) -> bool {
  std::ifstream in(path_, std::ios::binary);
  if (!in) {
    *error = absl::StrCat("cannot read client registry ", path_.string());
    return false;
  }
  const std::string text((std::istreambuf_iterator<char>(in)),
                         std::istreambuf_iterator<char>());

  proto::ClientRegistry parsed;
  if (!google::protobuf::TextFormat::ParseFromString(text, &parsed)) {
    *error = absl::StrCat("cannot parse client registry ", path_.string());
    return false;
  }
  for (const proto::Client &client : parsed.clients()) {
    if (client.client_id().empty()) {
      *error = "a client entry has no client_id";
      return false;
    }
    if (client.token_sha256().size() != 64) {
      *error = absl::StrCat("client '", client.client_id(),
                            "' has a token_sha256 that is not 64 hex chars; "
                            "mint one with arena_admin");
      return false;
    }
  }

  // Only swapped in once the whole file is known good, so a typo during a
  // reload leaves the running set intact rather than locking everyone out.
  {
    std::lock_guard lock(mutex_);
    registry_ = std::move(parsed);
  }
  LOG(INFO) << "Loaded " << size() << " client(s) from " << path_;
  return true;
}

auto ClientRegistry::Resolve(std::string_view token) const
    -> std::optional<ClientIdentity> {
  if (token.empty()) {
    return std::nullopt;
  }
  const std::string hashed = HashToken(token);

  std::lock_guard lock(mutex_);
  for (const proto::Client &client : registry_.clients()) {
    if (!ConstantTimeEquals(hashed,
                            absl::AsciiStrToLower(client.token_sha256()))) {
      continue;
    }
    if (client.disabled()) {
      return std::nullopt;
    }
    ClientIdentity identity;
    identity.client_id = client.client_id();
    identity.display_name = client.display_name().empty()
                                ? client.client_id()
                                : client.display_name();
    identity.quota = client.quota();
    if (identity.quota.max_active_evaluations() == 0) {
      identity.quota.set_max_active_evaluations(
          defaults_.max_active_evaluations());
    }
    if (identity.quota.max_queued_jobs() == 0) {
      identity.quota.set_max_queued_jobs(defaults_.max_queued_jobs());
    }
    return identity;
  }
  return std::nullopt;
}

auto ClientRegistry::size() const -> std::size_t {
  return static_cast<std::size_t>(registry_.clients_size());
}

}  // namespace tournament_arena
