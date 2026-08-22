// Harness around a submitted candidate strategy. Checked in once and compiled
// unchanged for every candidate: the arena points CANDIDATE_ENTRY_HEADER at
// the submitted header, which defines MakePolicy().
//
//   bazel run //game_mcts/tournament_server/candidate:dev_bot --
//       --name=me-dev --server=localhost:50051 --opponent=builtin:mcts
//       --games=5 --params=iterations=800
//
// Prints one machine-readable summary line the sandbox worker parses:
//
//   RESULT games=5 wins=3 draws=0 losses=2 elo=1512.4

#include <exception>
#include <random>
#include <string>

#include <grpcpp/grpcpp.h>

#include "absl/flags/flag.h"
#include "absl/flags/parse.h"
#include "absl/log/initialize.h"
#include "absl/log/log.h"
#include "cpp/tournament_server/candidate/candidate_api.h"
#include "cpp/tournament_server/remote_client.h"
#include "cpp/tournament_server/tournament_broker.grpc.pb.h"

#ifndef CANDIDATE_ENTRY_HEADER
#error "CANDIDATE_ENTRY_HEADER must name the candidate's strategy header"
#endif
#include CANDIDATE_ENTRY_HEADER

ABSL_FLAG(std::string, server, "localhost:50051", "host:port of the broker");
ABSL_FLAG(std::string, name, "", "Player name; the candidate id in the arena");
ABSL_FLAG(std::string, opponent, "any",
          "any | builtin:<spec> | player:<name>");
ABSL_FLAG(int, games, 1, "Number of games to play");
ABSL_FLAG(std::string, params, "",
          "Tuning knobs for MakePolicy, as key=value,key=value");
ABSL_FLAG(int, seed, 0, "RNG seed; 0 draws from the system entropy source");

auto main(int argc, char **argv) -> int {
  absl::ParseCommandLine(argc, argv);
  absl::InitializeLog();

  if (absl::GetFlag(FLAGS_name).empty()) {
    LOG(ERROR) << "Missing required --name=<player name>";
    return 2;
  }

  const int seed = absl::GetFlag(FLAGS_seed);
  std::mt19937 gen(seed != 0 ? static_cast<uint32_t>(seed)
                             : std::random_device{}());

  auto channel = grpc::CreateChannel(absl::GetFlag(FLAGS_server),
                                     grpc::InsecureChannelCredentials());
  auto stub = tournament_broker::proto::TournamentBroker::NewStub(channel);

  std::vector<tournament_broker::RemoteGameResult> results;
  try {
    // Built once and reused: a candidate may spend real work here (loading
    // tables, sizing buffers) that must not be repeated per game.
    candidate::policy_t policy =
        MakePolicy(candidate::Params::Parse(absl::GetFlag(FLAGS_params)));
    results = tournament_broker::PlayRemoteGames<candidate::game_t>(
        stub.get(), absl::GetFlag(FLAGS_name), std::string(candidate::kGameName),
        absl::GetFlag(FLAGS_opponent), absl::GetFlag(FLAGS_games), policy, gen);
  } catch (const std::exception &error) {
    LOG(ERROR) << error.what();
    return 1;
  }

  int wins = 0;
  int draws = 0;
  int losses = 0;
  double elo = 0.0;
  for (const auto &result : results) {
    switch (result.result) {
      case tournament_broker::proto::GameOver::WIN:
        ++wins;
        break;
      case tournament_broker::proto::GameOver::DRAW:
        ++draws;
        break;
      default:
        ++losses;
        break;
    }
    elo = result.new_elo;  // The last game's rating is the current one.
  }

  std::printf("RESULT games=%zu wins=%d draws=%d losses=%d elo=%.1f\n",
              results.size(), wins, draws, losses, elo);
  return 0;
}
