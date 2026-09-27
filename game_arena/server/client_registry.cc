#include "game_arena/server/client_registry.h"

#include <google/protobuf/text_format.h>
#include <openssl/mem.h>
#include <openssl/sha.h>

#include <cstddef>
#include <fstream>
#include <ios>
#include <iterator>
#include <random>
#include <string>
#include <utility>

#include "absl/log/log.h"
#include "absl/strings/ascii.h"
#include "absl/strings/escaping.h"
#include "absl/strings/str_cat.h"

namespace tournament_arena {

namespace {

// Constant-time, or a stopwatch would reveal a valid hash a byte at a time.
bool ConstantTimeEquals(std::string_view a, std::string_view b) {
  return a.size() == b.size() &&
         CRYPTO_memcmp(a.data(), b.data(), a.size()) == 0;
}

}  // namespace

std::string HashToken(std::string_view token) {
  unsigned char digest[SHA256_DIGEST_LENGTH];
  ::SHA256(reinterpret_cast<const unsigned char *>(token.data()), token.size(),
           digest);
  return absl::BytesToHexString(
      {reinterpret_cast<const char *>(digest), sizeof(digest)});
}

std::string MintToken() {
  // The system CSPRNG, never a seeded PRNG: this is a credential.
  std::random_device entropy;
  std::string bytes(32, '\0');
  for (char &byte : bytes) {
    byte = static_cast<char>(entropy());
  }
  return absl::BytesToHexString(bytes);
}

proto::Client MakeClient(std::string_view client_id,
                         std::string_view display_name, std::string_view token,
                         const proto::ClientQuota &quota) {
  proto::Client client;
  client.set_client_id(std::string(client_id));
  if (!display_name.empty()) {
    client.set_display_name(std::string(display_name));
  }
  client.set_token_sha256(HashToken(token));
  if (quota.max_active_evaluations() > 0 || quota.max_queued_jobs() > 0) {
    *client.mutable_quota() = quota;
  }
  return client;
}

std::string ClientBlockText(const proto::Client &client) {
  // Printed through the registry message: exactly the shape Load() parses.
  proto::ClientRegistry one;
  *one.add_clients() = client;
  std::string text;
  google::protobuf::TextFormat::PrintToString(one, &text);
  return text;
}

bool AppendClientToRegistry(const std::filesystem::path &path,
                            const proto::Client &client, std::string *error) {
  std::string existing;
  if (std::filesystem::exists(path)) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
      *error = absl::StrCat("cannot read client registry ", path.string());
      return false;
    }
    existing.assign((std::istreambuf_iterator<char>(in)),
                    std::istreambuf_iterator<char>());
    proto::ClientRegistry parsed;
    if (!google::protobuf::TextFormat::ParseFromString(existing, &parsed)) {
      *error = absl::StrCat("cannot parse client registry ", path.string(),
                            "; not appending to a file that does not load");
      return false;
    }
    for (const proto::Client &present : parsed.clients()) {
      if (present.client_id() == client.client_id()) {
        *error = absl::StrCat("client '", client.client_id(),
                              "' is already in ", path.string());
        return false;
      }
    }
  }
  std::ofstream out(path, std::ios::binary | std::ios::app);
  if (!out) {
    *error = absl::StrCat("cannot write client registry ", path.string());
    return false;
  }
  if (!existing.empty() && existing.back() != '\n') {
    out << '\n';
  }
  out << ClientBlockText(client);
  return static_cast<bool>(out);
}

bool SetClientToken(const std::filesystem::path &path,
                    const proto::Client &client, std::string *error) {
  if (!std::filesystem::exists(path)) {
    return AppendClientToRegistry(path, client, error);
  }
  std::ifstream in(path, std::ios::binary);
  const std::string existing((std::istreambuf_iterator<char>(in)),
                             std::istreambuf_iterator<char>());
  proto::ClientRegistry parsed;
  if (!in ||
      !google::protobuf::TextFormat::ParseFromString(existing, &parsed)) {
    *error = absl::StrCat("cannot read client registry ", path.string());
    return false;
  }
  bool found = false;
  for (proto::Client &present : *parsed.mutable_clients()) {
    if (present.client_id() == client.client_id()) {
      present.set_token_sha256(client.token_sha256());
      found = true;
    }
  }
  if (!found) {
    return AppendClientToRegistry(path, client, error);
  }
  std::string text;
  google::protobuf::TextFormat::PrintToString(parsed, &text);
  // Written aside and renamed, so a server reading it never sees half a file.
  const std::filesystem::path tmp = path.string() + ".tmp";
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    out << text;
    if (!out) {
      *error = absl::StrCat("cannot write ", tmp.string());
      return false;
    }
  }
  std::error_code ec;
  std::filesystem::rename(tmp, path, ec);
  if (ec) {
    *error = absl::StrCat("cannot replace ", path.string(), ": ", ec.message());
    return false;
  }
  return true;
}

ClientRegistry::ClientRegistry(std::filesystem::path path,
                               proto::ClientQuota defaults)
    : path_(std::move(path)), defaults_(std::move(defaults)) {}

bool ClientRegistry::Load(std::string *error) {
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

  {
    std::lock_guard lock(mutex_);
    registry_ = std::move(parsed);
  }
  LOG(INFO) << "Loaded " << size() << " client(s) from " << path_;
  return true;
}

std::optional<ClientIdentity> ClientRegistry::Resolve(
    std::string_view token) const {
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
    identity.quota = defaults_;
    identity.quota.MergeFrom(client.quota());
    return identity;
  }
  return std::nullopt;
}

std::size_t ClientRegistry::size() const {
  return static_cast<std::size_t>(registry_.clients_size());
}

}  // namespace tournament_arena
