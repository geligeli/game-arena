#include "game_arena/client/play_loop.h"

#include <grpcpp/test/mock_stream.h>

#include <random>
#include <string>

#include "game_arena/proto/tournament_broker_mock.grpc.pb.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace tournament_client {
namespace {

using ::testing::_;
using ::testing::DoAll;
using ::testing::Return;
using ::testing::SetArgPointee;
using tournament_broker::proto::ClientMessage;
using tournament_broker::proto::ServerMessage;
using Stream =
    grpc::testing::MockClientReaderWriter<ClientMessage, ServerMessage>;

// The server ended the game while the bot was thinking: its action's write is
// refused, and the game_over is still waiting to be read.
Stream *LateActionGame() {
  ServerMessage turn;
  turn.mutable_your_turn();
  ServerMessage over;
  over.mutable_game_over()->set_reason("timeout");
  auto *stream = new Stream();
  EXPECT_CALL(*stream, Write(_, _))
      .WillOnce(Return(true))    // hello
      .WillOnce(Return(false));  // the late action
  EXPECT_CALL(*stream, Read(_))
      .WillOnce(DoAll(SetArgPointee<0>(turn), Return(true)))
      .WillOnce(DoAll(SetArgPointee<0>(over), Return(true)));
  EXPECT_CALL(*stream, WritesDone()).WillOnce(Return(true));
  EXPECT_CALL(*stream, Finish()).WillOnce(Return(grpc::Status::OK));
  return stream;
}

TEST(PlayLoopTest, LateActionLosesOneGameNotTheSeries) {
  tournament_broker::proto::MockTournamentBrokerStub stub;
  EXPECT_CALL(stub, PlayRaw(_))
      .WillOnce(LateActionGame)
      .WillOnce(LateActionGame);
  std::mt19937 gen(0);
  EXPECT_TRUE(PlayGames(
      &stub, "me", "nim", "builtin:random", 2,
      [](std::string_view, std::mt19937 &) { return std::string("1"); }, gen));
}

}  // namespace
}  // namespace tournament_client
