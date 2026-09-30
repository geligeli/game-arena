// The coordinator. It links no problem code (:no_problem_code_test).

#include <grpcpp/grpcpp.h>
#include <signal.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

#include "absl/flags/flag.h"
#include "absl/flags/parse.h"
#include "absl/log/globals.h"
#include "absl/log/initialize.h"
#include "absl/log/log.h"
#include "game_arena/server/arena_service.h"
#include "game_arena/server/artifact_store.h"
#include "game_arena/server/candidate_store.h"
#include "game_arena/server/client_registry.h"
#include "game_arena/server/dashboard.h"
#include "game_arena/server/fleet_service.h"
#include "game_arena/server/job_log.h"
#include "game_arena/server/matchmaking.h"
#include "game_arena/server/problem_config.h"
#include "game_arena/server/scheduler.h"
#include "game_arena/server/swiss.h"
#include "game_arena/standings/elo_standings.h"
#include "game_arena/standings/elo_store.h"
#include "game_arena/standings/game_history.h"
#include "game_arena/standings/http_leaderboard.h"
#include "game_arena/standings/metric_standings.h"
#include "game_arena/standings/trueskill.h"
#include "game_arena/standings/trueskill_standings.h"

ABSL_FLAG(std::string, problem_config, "",
          "Path to the problem's .textproto (required). See "
          "game_arena/problems/ for the shipped ones");
ABSL_FLAG(int, grpc_port, 50051,
          "Port for the Arena and SandboxFleet services");
ABSL_FLAG(int, http_port, 8090, "Port for the HTTP leaderboard");
ABSL_FLAG(std::string, data_dir, "tournament_data",
          "Directory for submissions, ratings.pb and games/");
ABSL_FLAG(std::string, clients, "",
          "Path to the client registry (.textproto). Writes require an "
          "x-arena-token header naming a client in it; reads never do. Empty "
          "leaves Submit open to anyone who can reach the port, "
          "which is fine for a single-agent loop and nothing else. Mint "
          "clients with //game_arena/tools:arena_admin");
ABSL_FLAG(double, k_factor, 32.0, "ELO K factor");
ABSL_FLAG(int, keepalive_s, 60,
          "Interval between HTTP/2 keepalive pings on idle connections. "
          "Reclaims connections whose peer vanished without a TCP FIN, which "
          "otherwise linger indefinitely");
ABSL_FLAG(std::vector<std::string>, replay_assets, {},
          "Files replay pages may load, served at /assets/<file name>: the "
          "problem's own (arena_problem's replay_assets), of any kind");
ABSL_FLAG(std::string, replay_module, "",
          "The ES module among --replay_assets that draws a replay's views. "
          "Empty: views are shown as text");
ABSL_FLAG(std::string, swiss_from, "",
          "Instead of taking submissions, re-rank every version in this "
          "tournament data dir's job log, and the builtins, in Swiss rounds "
          "rated by TrueSkill; progress at /swiss. Reads it, never writes it");
ABSL_FLAG(int, swiss_rounds, 0,
          "Swiss rounds in all: a restart on the same --data_dir resumes "
          "after the ones already played. 0: ceil(log2(entries)) + 3");
ABSL_FLAG(int, swiss_games, 0,
          "Games per Swiss match. 0: one for each order of the seats, so "
          "each side has each seat as often (2 for two seats, 6 for three)");
ABSL_FLAG(std::string, import_versions_from, "",
          "With submission.versions: before serving, make every submission in "
          "this data dir's job log that played a game a READY version here. "
          "For moving a tournament to versions; reads it, never writes it");
ABSL_FLAG(int, shutdown_grace_s, 5,
          "How long a shutdown waits for in-flight RPCs to finish before "
          "cancelling them");

namespace {

// --replay_assets and --replay_module, read once; nullopt, with *error, when
// a file is missing, two share a name, or the module is not among them.
std::optional<tournament_arena::ReplayAssets> LoadReplayAssets(
    std::string* error) {
  tournament_arena::ReplayAssets assets;
  assets.module = absl::GetFlag(FLAGS_replay_module);
  for (const std::string& path : absl::GetFlag(FLAGS_replay_assets)) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
      *error = "cannot read replay asset " + path;
      return std::nullopt;
    }
    std::string bytes((std::istreambuf_iterator<char>(in)),
                      std::istreambuf_iterator<char>());
    const std::string name = std::filesystem::path(path).filename().string();
    if (!assets.files.emplace(name, std::move(bytes)).second) {
      *error = "two replay assets are named " + name;
      return std::nullopt;
    }
  }
  if (!assets.module.empty() && !assets.files.contains(assets.module)) {
    *error = "replay module " + assets.module + " is not among --replay_assets";
    return std::nullopt;
  }
  return assets;
}

}  // namespace

int main(int argc, char** argv) {
  // Before any thread starts, so only the sigwait below sees them.
  sigset_t shutdown_signals;
  sigemptyset(&shutdown_signals);
  sigaddset(&shutdown_signals, SIGINT);
  sigaddset(&shutdown_signals, SIGTERM);
  pthread_sigmask(SIG_BLOCK, &shutdown_signals, nullptr);

  absl::ParseCommandLine(argc, argv);
  absl::InitializeLog();
  absl::SetStderrThreshold(absl::LogSeverityAtLeast::kInfo);

  const std::string config_path = absl::GetFlag(FLAGS_problem_config);
  if (config_path.empty()) {
    LOG(ERROR) << "--problem_config is required";
    return 2;
  }
  std::string error;
  auto problem = tournament_arena::LoadProblemConfig(config_path, &error);
  if (!problem) {
    LOG(ERROR) << error;
    return 2;
  }
  const std::filesystem::path data_dir = absl::GetFlag(FLAGS_data_dir);
  std::error_code ec;
  std::filesystem::create_directories(data_dir, ec);
  if (ec) {
    LOG(ERROR) << "Cannot create --data_dir " << data_dir << ": "
               << ec.message();
    return 1;
  }

  tournament_broker::EloStore elo_store(data_dir / "ratings.pb",
                                        absl::GetFlag(FLAGS_k_factor));
  elo_store.Load();
  tournament_broker::GameHistory history(data_dir / "games");

  tournament_arena::CandidateStore candidates(
      data_dir / "candidates", tournament_arena::CandidateLimits{},
      problem->submission());
  candidates.Load();

  const std::filesystem::path swiss_from = absl::GetFlag(FLAGS_swiss_from);
  tournament_broker::trueskill::Params trueskill_params;
  trueskill_params.draw_probability = problem->ranking().draw_probability();
  std::vector<tournament_arena::SwissEntry> swiss_entries;
  if (!swiss_from.empty()) {
    tournament_broker::GameHistory board_games(swiss_from / "games");
    const tournament_arena::TrueSkillStandings board(board_games, nullptr,
                                                     trueskill_params);
    swiss_entries = tournament_arena::SeedVersions(
        tournament_arena::JobLog(swiss_from / "jobs"),
        problem->submission().files_submit_dir(), board, &candidates);
    for (const std::string& builtin : problem->match().placement_opponents()) {
      swiss_entries.emplace_back().id = builtin;
    }
    // No dynamics: a version's code never changes, so every game is evidence.
    trueskill_params.tau = 0;
  }
  if (problem->submission().versions()) {
    trueskill_params.tau = 0;
  }
  if (const std::filesystem::path from =
          absl::GetFlag(FLAGS_import_versions_from);
      !from.empty()) {
    tournament_broker::GameHistory old_games(from / "games");
    const tournament_arena::TrueSkillStandings board(old_games, nullptr,
                                                     trueskill_params);
    const auto imported = tournament_arena::SeedVersions(
        tournament_arena::JobLog(from / "jobs"),
        problem->submission().files_submit_dir(), board, &candidates);
    LOG(INFO) << "Imported " << imported.size() << " version(s) from " << from;
  }

  std::unique_ptr<tournament_arena::Standings> standings;
  const tournament_arena::TrueSkillStandings* ratings = nullptr;
  std::unique_ptr<tournament_arena::MetricStandings> metric_standings;
  const bool graded = problem->has_grade();
  if (graded) {
    const tournament_arena::proto::MetricSpec* primary =
        tournament_arena::PrimaryMetric(*problem);
    metric_standings = std::make_unique<tournament_arena::MetricStandings>(
        data_dir / "metrics.pb", primary->name(),
        primary->direction() == tournament_arena::proto::MetricSpec::MINIMIZE);
    metric_standings->Load();
    standings = std::move(metric_standings);
  } else if (!swiss_from.empty() ||
             problem->ranking().kind() ==
                 tournament_arena::proto::RankingSpec::TRUESKILL) {
    auto trueskill = std::make_unique<tournament_arena::TrueSkillStandings>(
        history, &candidates, trueskill_params);
    ratings = trueskill.get();
    standings = std::move(trueskill);
  } else {
    standings = std::make_unique<tournament_arena::EloStandings>(
        &elo_store, &candidates, problem->problem_id());
  }

  std::unique_ptr<tournament_arena::ClientRegistry> clients;
  if (!absl::GetFlag(FLAGS_clients).empty()) {
    clients = std::make_unique<tournament_arena::ClientRegistry>(
        absl::GetFlag(FLAGS_clients), problem->clients().default_quota());
    if (!clients->Load(&error)) {
      LOG(ERROR) << error;
      return 2;
    }
  } else {
    LOG(WARNING) << "No --clients registry: Submit is open to "
                    "anyone who can reach this port, and `author` is whatever "
                    "the caller says it is";
  }

  tournament_arena::JobLog job_log(data_dir / "jobs");
  tournament_arena::ArtifactStore artifacts(data_dir / "artifacts");
  std::unique_ptr<tournament_arena::SwissRun> swiss;
  std::unique_ptr<tournament_arena::Matchmaker> matchmaker;
  tournament_arena::Scheduler scheduler(
      tournament_arena::SchedulerConfigFor(*problem), &candidates,
      standings.get(), &history, &job_log,
      [&swiss, &matchmaker](const tournament_arena::proto::Job& job) {
        if (swiss) {
          swiss->OnConcluded(job);
        }
        if (matchmaker) {
          matchmaker->OnConcluded(job);
        }
      },
      // A graded problem builds in its one order, beside the tree it reads.
      graded ? nullptr : &artifacts);
  if (problem->match().has_matchmaking() && swiss_from.empty()) {
    matchmaker = std::make_unique<tournament_arena::Matchmaker>(
        problem->match().matchmaking(), problem->match().game(),
        static_cast<int>(problem->match().players()),
        std::vector<std::string>(problem->match().placement_opponents().begin(),
                                 problem->match().placement_opponents().end()),
        &scheduler, &candidates, ratings, trueskill_params,
        data_dir / "matchmaking.tsv");
  }
  // Nothing in flight survives a restart: close what the log still has open,
  // and place again whatever was waiting on its placement.
  for (tournament_arena::JobRecord record : job_log.List()) {
    auto* job = record.mutable_job();
    if (job->state() == tournament_arena::proto::Job::QUEUED ||
        job->state() == tournament_arena::proto::Job::RUNNING) {
      job->set_state(tournament_arena::proto::Job::CANCELLED);
      job->set_error("interrupted by a restart");
      job_log.Put(record);
    }
  }
  for (const auto& candidate : candidates.List()) {
    if (candidate.status() == tournament_arena::proto::Candidate::PENDING) {
      // Unmetered, like a match: the client asked once already.
      std::string error;
      auto reservation =
          scheduler.TryReserve("", {}, /*cancel_running=*/false, &error);
      LOG(INFO) << "Placing " << candidate.candidate_id() << " again as "
                << scheduler.EnqueuePlacement(candidate,
                                              std::move(*reservation));
    }
  }
  // Curated: the operator's image names and timeouts are not a submitter's.
  tournament_arena::proto::ProblemInfo info;
  info.set_problem_id(problem->problem_id());
  info.set_display_name(problem->display_name());
  info.set_description(problem->description());
  info.set_max_patch_bytes(problem->submission().max_patch_bytes());
  info.set_max_files(problem->submission().max_files());
  info.set_max_hunks(problem->submission().max_hunks());
  *info.mutable_allow_paths() = problem->submission().allow_paths();
  *info.mutable_deny_paths() = problem->submission().deny_paths();
  info.set_files_submit_dir(problem->submission().files_submit_dir());
  const auto visibility = problem->source().visibility();
  info.set_source_visibility(
      visibility == tournament_arena::proto::SourcePolicy::OWN
          ? tournament_arena::proto::ProblemInfo::SOURCE_OWN
      : visibility == tournament_arena::proto::SourcePolicy::NONE
          ? tournament_arena::proto::ProblemInfo::SOURCE_NONE
          : tournament_arena::proto::ProblemInfo::SOURCE_ALL);
  info.set_graded(graded);
  if (graded) {
    const auto* primary = tournament_arena::PrimaryMetric(*problem);
    info.set_lower_is_better(primary->direction() ==
                             tournament_arena::proto::MetricSpec::MINIMIZE);
  }

  tournament_arena::ArenaService arena(
      &candidates, &scheduler, standings.get(),
      problem->has_match() ? problem->match().game() : "", std::move(info),
      clients.get());
  tournament_arena::FleetService fleet(&scheduler, &artifacts);

  grpc::ServerBuilder builder;
  builder.AddListeningPort(
      "0.0.0.0:" + std::to_string(absl::GetFlag(FLAGS_grpc_port)),
      grpc::InsecureServerCredentials());
  // A host powered off mid-order sends no FIN; keepalive reclaims its stream.
  const int keepalive_ms = absl::GetFlag(FLAGS_keepalive_s) * 1000;
  builder.AddChannelArgument(GRPC_ARG_KEEPALIVE_TIME_MS, keepalive_ms);
  builder.AddChannelArgument(GRPC_ARG_KEEPALIVE_TIMEOUT_MS, 20000);
  builder.AddChannelArgument(GRPC_ARG_KEEPALIVE_PERMIT_WITHOUT_CALLS, 1);
  // A worker with no free slots is idle, not dead.
  builder.AddChannelArgument(
      GRPC_ARG_HTTP2_MIN_RECV_PING_INTERVAL_WITHOUT_DATA_MS, keepalive_ms / 2);
  // A game's record arrives whole, as one OrderGame, and its views are
  // uncapped: gRPC's default 4 MB would drop a long game's.
  builder.SetMaxReceiveMessageSize(64 << 20);
  // A re-rank takes no submissions: only workers dial it.
  if (swiss_from.empty()) {
    builder.RegisterService(&arena);
  }
  builder.RegisterService(&fleet);
  std::unique_ptr<grpc::Server> server = builder.BuildAndStart();
  if (!server) {
    LOG(ERROR) << "Cannot bind gRPC port " << absl::GetFlag(FLAGS_grpc_port);
    return 1;
  }

  std::string assets_error;
  std::optional<tournament_arena::ReplayAssets> assets =
      LoadReplayAssets(&assets_error);
  if (!assets.has_value()) {
    LOG(ERROR) << assets_error;
    return 1;
  }
  const tournament_arena::Dashboard dashboard(
      &candidates, &job_log, &history, standings.get(),
      problem->source().visibility() ==
          tournament_arena::proto::SourcePolicy::ALL,
      std::move(*assets), static_cast<int>(problem->match().players()));
  tournament_broker::HttpLeaderboard leaderboard(
      absl::GetFlag(FLAGS_http_port), &history, &candidates, standings.get(),
      problem->display_name().empty() ? problem->problem_id()
                                      : problem->display_name(),
      [&dashboard, &swiss, &matchmaker](std::string_view target) {
        if (swiss) {
          if (auto page = swiss->Route(target)) {
            return page;
          }
        }
        if (matchmaker) {
          if (auto page = matchmaker->Route(target)) {
            return page;
          }
        }
        return dashboard.Route(target);
      });
  leaderboard.set_players(static_cast<int>(problem->match().players()));
  if (matchmaker) {
    leaderboard.set_pool([&matchmaker] { return matchmaker->Members(); });
  }
  if (!leaderboard.Start()) {
    return 1;
  }

  LOG(INFO) << "Problem '" << problem->problem_id() << "' ("
            << (problem->has_match() ? "match" : "grade")
            << ") on :" << absl::GetFlag(FLAGS_grpc_port) << ", leaderboard on "
            << "http://localhost:" << absl::GetFlag(FLAGS_http_port)
            << ", data dir " << data_dir << ", sandbox image "
            << problem->sandbox().image() << ", " << candidates.size()
            << " submission(s) loaded";
  if (problem->source().visibility() !=
      tournament_arena::proto::SourcePolicy::ALL) {
    LOG(INFO) << "Candidate source is "
              << (problem->source().visibility() ==
                          tournament_arena::proto::SourcePolicy::OWN
                      ? "served only to its own author, on a token"
                      : "served to nobody")
              << ": the default is that every participant reads every "
                 "submission";
  }
  if (!swiss_from.empty()) {
    const int seats = static_cast<int>(problem->match().players());
    int games = absl::GetFlag(FLAGS_swiss_games);
    if (games <= 0) {
      games = 1;
      for (int i = 2; i <= seats; ++i) {
        games *= i;
      }
    }
    swiss = std::make_unique<tournament_arena::SwissRun>(
        std::move(swiss_entries), absl::GetFlag(FLAGS_swiss_rounds), games,
        seats, problem->match().game(), &scheduler, &candidates, ratings,
        &job_log, data_dir / "swiss.tsv");
    swiss->Start();
    LOG(INFO) << "Swiss re-rank of " << swiss_from
              << ": http://localhost:" << absl::GetFlag(FLAGS_http_port)
              << "/swiss";
  }

  if (matchmaker) {
    matchmaker->Start();
    LOG(INFO) << "Matchmaking: http://localhost:"
              << absl::GetFlag(FLAGS_http_port) << "/pool";
  }

  int signum = 0;
  sigwait(&shutdown_signals, &signum);
  LOG(INFO) << "Shutting down";

  // With no deadline, Shutdown() would wait forever on a worker's Attach.
  server->Shutdown(std::chrono::system_clock::now() +
                   std::chrono::seconds(absl::GetFlag(FLAGS_shutdown_grace_s)));
  leaderboard.Stop();
  swiss.reset();
  matchmaker.reset();
  return 0;
}
