// The harness every submission is compiled into.
//
// It owns the whole protocol so a submitter does not have to: the arena's
// play_loop runs the Play stream, this file turns the bytes into a bot::Board,
// and CANDIDATE_ENTRY_HEADER supplies ChooseColumn. The submission never sees
// gRPC.
//
// The coordinator generates a BUILD per submission that compiles exactly this
// source with CANDIDATE_ENTRY_HEADER pointing at the submitted header -- see
// submission.harness in problem.textproto.

#include <grpcpp/grpcpp.h>

#include <random>
#include <string>

#include "absl/flags/flag.h"
#include "absl/flags/parse.h"
#include "absl/log/globals.h"
#include "absl/log/initialize.h"
#include "absl/log/log.h"
#include "bots/bot_api.h"
#include "game/connect4.h"
#include "game_arena/client/play_loop.h"
#include "game_arena/proto/tournament_broker.grpc.pb.h"

#ifndef CANDIDATE_ENTRY_HEADER
#error "compile with -DCANDIDATE_ENTRY_HEADER=\"path/to/strategy.h\""
#endif
#include CANDIDATE_ENTRY_HEADER

ABSL_FLAG(std::string, server, "localhost:50051", "host:port of the broker");
ABSL_FLAG(std::string, name, "", "Player name (required)");
ABSL_FLAG(std::string, opponent, "any",
          "any | builtin:random | builtin:greedy | player:<name>");
ABSL_FLAG(int, games, 1, "Number of games to play");

auto main(int argc, char **argv) -> int {
  absl::ParseCommandLine(argc, argv);
  absl::InitializeLog();
  // Without this the per-game result lines are compiled in but never shown,
  // and a submitter iterating locally sees an empty terminal.
  absl::SetStderrThreshold(absl::LogSeverityAtLeast::kInfo);

  const std::string name = absl::GetFlag(FLAGS_name);
  if (name.empty()) {
    LOG(ERROR) << "Missing required --name=<player name>";
    return 1;
  }

  auto channel = grpc::CreateChannel(absl::GetFlag(FLAGS_server),
                                     grpc::InsecureChannelCredentials());
  auto stub = tournament_broker::proto::TournamentBroker::NewStub(channel);

  const auto choose = [](std::string_view state_bytes,
                         std::mt19937 &gen) -> std::string {
    bot::Board board;
    board.cells = connect4::EmptyBoard();
    if (!connect4::ParseBoard(state_bytes, &board.cells, &board.seat)) {
      LOG(ERROR) << "could not parse the state; forfeiting this move";
      return "0";
    }
    board.me = connect4::DiscFor(board.seat);
    board.them = connect4::DiscFor(1 - board.seat);
    return std::to_string(ChooseColumn(board, gen));
  };

  std::mt19937 gen(std::random_device{}());
  return tournament_client::PlayGames(stub.get(), name, "connect4",
                                      absl::GetFlag(FLAGS_opponent),
                                      absl::GetFlag(FLAGS_games), choose, gen)
             ? 0
             : 1;
}
