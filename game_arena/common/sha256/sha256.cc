#include "game_arena/common/sha256/sha256.h"

#include <openssl/sha.h>

#include "absl/strings/escaping.h"

namespace sha256 {

std::string Hex(std::string_view data) {
  unsigned char digest[SHA256_DIGEST_LENGTH];
  ::SHA256(reinterpret_cast<const unsigned char *>(data.data()), data.size(),
           digest);
  return absl::BytesToHexString(
      {reinterpret_cast<const char *>(digest), sizeof(digest)});
}

}  // namespace sha256
