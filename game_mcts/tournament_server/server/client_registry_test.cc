#include "game_mcts/tournament_server/server/client_registry.h"

#include <gtest/gtest.h>
#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <string>

namespace tournament_arena {
namespace {

class ClientRegistryTest : public ::testing::Test {
 protected:
  void SetUp() override {
    dir_ = std::filesystem::temp_directory_path() /
           ("client_registry_" + std::to_string(::getpid()) + "_" +
            std::to_string(::testing::UnitTest::GetInstance()->random_seed()));
    std::filesystem::remove_all(dir_);
    std::filesystem::create_directories(dir_);
    path_ = dir_ / "clients.textproto";
  }
  void TearDown() override { std::filesystem::remove_all(dir_); }

  void WriteRegistry(const std::string &text) {
    std::ofstream out(path_);
    out << text;
  }

  auto Defaults() -> proto::ClientQuota {
    proto::ClientQuota quota;
    quota.set_max_active_evaluations(1);
    quota.set_max_queued_jobs(8);
    return quota;
  }

  std::filesystem::path dir_;
  std::filesystem::path path_;
};

TEST_F(ClientRegistryTest, HashesAreStableAndTokenSpecific) {
  // The admin tool and the server must agree on the encoding by construction.
  EXPECT_EQ(HashToken("abc"),
            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  EXPECT_EQ(HashToken("").size(), 64u);
  EXPECT_NE(HashToken("token-a"), HashToken("token-b"));
}

TEST_F(ClientRegistryTest, ResolvesAKnownToken) {
  WriteRegistry(
      "clients { client_id: \"agent-1\" display_name: \"Agent One\" "
      "token_sha256: \"" +
      HashToken("s3cret") + "\" }");
  ClientRegistry registry(path_, Defaults());
  std::string error;
  ASSERT_TRUE(registry.Load(&error)) << error;

  const auto identity = registry.Resolve("s3cret");
  ASSERT_TRUE(identity.has_value());
  EXPECT_EQ(identity->client_id, "agent-1");
  EXPECT_EQ(identity->display_name, "Agent One");
  // Unset quota fields fall back to the problem's defaults.
  EXPECT_EQ(identity->quota.max_active_evaluations(), 1u);
  EXPECT_EQ(identity->quota.max_queued_jobs(), 8u);
}

TEST_F(ClientRegistryTest, RefusesUnknownEmptyAndDisabledTokens) {
  WriteRegistry("clients { client_id: \"live\" token_sha256: \"" +
                HashToken("good") +
                "\" }\n"
                "clients { client_id: \"retired\" disabled: true "
                "token_sha256: \"" +
                HashToken("revoked") + "\" }");
  ClientRegistry registry(path_, Defaults());
  std::string error;
  ASSERT_TRUE(registry.Load(&error)) << error;

  EXPECT_TRUE(registry.Resolve("good").has_value());
  EXPECT_FALSE(registry.Resolve("wrong").has_value());
  EXPECT_FALSE(registry.Resolve("").has_value());
  // Disabled keeps the id and its history while refusing new work; deleting
  // the entry would orphan everything it authored.
  EXPECT_FALSE(registry.Resolve("revoked").has_value());
}

TEST_F(ClientRegistryTest, PerClientQuotaOverridesTheDefault) {
  WriteRegistry("clients { client_id: \"heavy\" token_sha256: \"" +
                HashToken("t") + "\" quota { max_active_evaluations: 4 } }");
  ClientRegistry registry(path_, Defaults());
  std::string error;
  ASSERT_TRUE(registry.Load(&error)) << error;

  const auto identity = registry.Resolve("t");
  ASSERT_TRUE(identity.has_value());
  EXPECT_EQ(identity->quota.max_active_evaluations(), 4u);
  // The field it did not override still falls back.
  EXPECT_EQ(identity->quota.max_queued_jobs(), 8u);
}

// Adding a client should not require a restart that drops every attached
// worker mid-order.
TEST_F(ClientRegistryTest, ReloadPicksUpANewClient) {
  WriteRegistry("clients { client_id: \"first\" token_sha256: \"" +
                HashToken("one") + "\" }");
  ClientRegistry registry(path_, Defaults());
  std::string error;
  ASSERT_TRUE(registry.Load(&error)) << error;
  EXPECT_FALSE(registry.Resolve("two").has_value());

  WriteRegistry("clients { client_id: \"first\" token_sha256: \"" +
                HashToken("one") +
                "\" }\n"
                "clients { client_id: \"second\" token_sha256: \"" +
                HashToken("two") + "\" }");
  ASSERT_TRUE(registry.Load(&error)) << error;
  EXPECT_TRUE(registry.Resolve("one").has_value());
  EXPECT_TRUE(registry.Resolve("two").has_value());
  EXPECT_EQ(registry.size(), 2u);
}

// A typo during a reload must not lock everyone out.
TEST_F(ClientRegistryTest, AFailedReloadKeepsTheRunningSet) {
  WriteRegistry("clients { client_id: \"first\" token_sha256: \"" +
                HashToken("one") + "\" }");
  ClientRegistry registry(path_, Defaults());
  std::string error;
  ASSERT_TRUE(registry.Load(&error)) << error;

  WriteRegistry("clients { client_id: \"broken\" toke_sha256: \"typo\" }");
  EXPECT_FALSE(registry.Load(&error));
  EXPECT_TRUE(registry.Resolve("one").has_value())
      << "a bad reload must not revoke working tokens";

  // A hash that is not a hash is caught rather than silently never matching.
  WriteRegistry("clients { client_id: \"short\" token_sha256: \"abc\" }");
  EXPECT_FALSE(registry.Load(&error));
  EXPECT_NE(error.find("64 hex"), std::string::npos) << error;

  WriteRegistry("clients { display_name: \"nameless\" token_sha256: \"" +
                HashToken("x") + "\" }");
  EXPECT_FALSE(registry.Load(&error));
  EXPECT_NE(error.find("client_id"), std::string::npos) << error;
}

TEST_F(ClientRegistryTest, MissingFileIsAnError) {
  ClientRegistry registry(dir_ / "absent.textproto", Defaults());
  std::string error;
  EXPECT_FALSE(registry.Load(&error));
  EXPECT_NE(error.find("absent.textproto"), std::string::npos) << error;
}

TEST_F(ClientRegistryTest, AcceptsAnUppercaseHashInTheFile) {
  std::string upper = HashToken("t");
  for (char &c : upper) {
    c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  }
  WriteRegistry("clients { client_id: \"a\" token_sha256: \"" + upper + "\" }");
  ClientRegistry registry(path_, Defaults());
  std::string error;
  ASSERT_TRUE(registry.Load(&error)) << error;
  EXPECT_TRUE(registry.Resolve("t").has_value())
      << "hex case is not part of the secret";
}

}  // namespace
}  // namespace tournament_arena
