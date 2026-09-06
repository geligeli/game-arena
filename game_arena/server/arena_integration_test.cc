// End-to-end through the arena's control plane over real gRPC: submit a
// candidate, watch a worker receive the order, report a result, and see the
// standings move.
//
// The worker here is a stub that fakes the build and the games. That is the
// point: everything between "an agent submits" and "a result comes back" is
// exercised without a git clone, a bazel build, or a real match, so this test
// runs in milliseconds and fails for reasons in this repo's control plane
// rather than in a toolchain.

#include <grpcpp/grpcpp.h>
#include <unistd.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>

#include "game_arena/proto/arena.grpc.pb.h"
#include "game_arena/server/arena_service.h"
#include "game_arena/server/candidate_store.h"
#include "game_arena/server/client_registry.h"
#include "game_arena/server/elo_standings.h"
#include "game_arena/server/elo_store.h"
#include "game_arena/server/fleet_service.h"
#include "game_arena/server/scheduler.h"
#include "gtest/gtest.h"

namespace tournament_arena {
namespace {

using Stream =
    grpc::ClientReaderWriter<proto::WorkerMessage, proto::FleetMessage>;

class ArenaIntegrationTest : public ::testing::Test {
 protected:
  void SetUp() override {
    dir_ = std::filesystem::temp_directory_path() /
           ("arena_it_" + std::to_string(::getpid()) + "_" +
            std::to_string(::testing::UnitTest::GetInstance()->random_seed()));
    std::filesystem::remove_all(dir_);

    SubmissionRules rules;
    rules.files_submit_dir = "solutions";
    rules.policy.add_allowed_dep_prefixes("//problem/lib:");
    rules.policy.add_allowed_dep_prefixes("//problem/lib:");
    rules.policy.add_allowed_dep_prefixes("//problem/game:");
    rules.policy.add_allowed_dep_prefixes("//problem/strategies:");
    rules.policy.add_allowed_dep_prefixes("@abseil-cpp//");
    rules.harness.set_api_dep("//problem/harness:api");
    rules.harness.set_main_src("//problem/harness:main.cc");
    rules.policy.add_allow_paths("solutions/**");
    store_ = std::make_unique<CandidateStore>(dir_ / "candidates",
                                              CandidateLimits{}, rules);
    elo_ = std::make_unique<tournament_broker::EloStore>(dir_ / "ratings.pb",
                                                         32.0);
    SchedulerConfig config;
    config.placement_opponents = {"builtin:random"};
    config.placement_games = 2;
    config.referee_target =
        "//game_arena/referee:match_referee";
    config.build_targets = {"//game_arena/candidates/{submission_id}:bot"};
    config.bot_target = "//game_arena/candidates/{submission_id}:bot";
    standings_ =
        std::make_unique<EloStandings>(elo_.get(), store_.get(), "risk2");
    scheduler_ = std::make_unique<Scheduler>(config, store_.get(), elo_.get(),
                                             standings_.get());
    // Null by default: most of these tests are about the arena's behaviour,
    // not its gate. The authenticated fixture below overrides it.
    clients_ = MakeClients();
    arena_ = std::make_unique<ArenaService>(
        store_.get(), scheduler_.get(), standings_.get(), "deadbeef",
        /*graded=*/false, proto::ProblemInfo{}, clients_.get());
    fleet_ = std::make_unique<FleetService>(scheduler_.get());

    grpc::ServerBuilder builder;
    builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(),
                             &port_);
    builder.RegisterService(arena_.get());
    builder.RegisterService(fleet_.get());
    server_ = builder.BuildAndStart();
    ASSERT_NE(server_, nullptr);

    channel_ = grpc::CreateChannel("127.0.0.1:" + std::to_string(port_),
                                   grpc::InsecureChannelCredentials());
    arena_stub_ = proto::Arena::NewStub(channel_);
    fleet_stub_ = proto::SandboxFleet::NewStub(channel_);
  }

  // Supplies the client registry the service is built with. Default: none, so
  // writes are ungated.
  virtual auto MakeClients() -> std::unique_ptr<ClientRegistry> {
    return nullptr;
  }

  void TearDown() override {
    server_->Shutdown(std::chrono::system_clock::now() +
                      std::chrono::seconds(2));
    server_.reset();
    std::filesystem::remove_all(dir_);
  }

  auto Submit(const std::string &name, const std::string &content = "// bot\n")
      -> proto::SubmitResponse {
    proto::SubmitRequest request;
    request.set_display_name(name);
    request.set_author("agent-1");
    request.set_game("risk2");
    request.set_entry_header("strategy.h");
    auto *file = request.add_files();
    file->set_path("strategy.h");
    file->set_content(content);

    grpc::ClientContext context;
    proto::SubmitResponse response;
    const grpc::Status status =
        arena_stub_->Submit(&context, request, &response);
    EXPECT_TRUE(status.ok()) << status.error_message();
    return response;
  }

  // Attaches a worker and returns its stream. The caller drives it, so a test
  // can decide exactly when an order is answered.
  auto AttachWorker(const std::string &id, int slots,
                    grpc::ClientContext *context) -> std::unique_ptr<Stream> {
    auto stream = fleet_stub_->Attach(context);
    proto::WorkerMessage hello;
    hello.mutable_hello()->set_worker_id(id);
    hello.mutable_hello()->set_slots(slots);
    hello.mutable_hello()->set_backend("stub");
    EXPECT_TRUE(stream->Write(hello));
    return stream;
  }

  static void ReportSuccess(Stream *stream, const std::string &order_id,
                            int wins, int losses, double elo) {
    proto::WorkerMessage msg;
    auto *result = msg.mutable_result();
    result->set_order_id(order_id);
    result->set_build_ok(true);
    result->set_games_played(wins + losses);
    result->set_wins(wins);
    result->set_losses(losses);
    result->set_elo(elo);
    EXPECT_TRUE(stream->Write(msg));
  }

  auto WaitForJob(const std::string &job_id,
                  proto::Job::State state) -> proto::Job {
    proto::Job job;
    for (int i = 0; i < 400; ++i) {
      grpc::ClientContext context;
      proto::GetJobRequest request;
      request.set_job_id(job_id);
      const grpc::Status status = arena_stub_->GetJob(&context, request, &job);
      EXPECT_TRUE(status.ok()) << status.error_message();
      if (job.state() == state) {
        return job;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    EXPECT_EQ(job.state(), state) << "job " << job_id << " never reached state";
    return job;
  }

  std::filesystem::path dir_;
  std::unique_ptr<CandidateStore> store_;
  std::unique_ptr<tournament_broker::EloStore> elo_;
  std::unique_ptr<ClientRegistry> clients_;
  std::unique_ptr<EloStandings> standings_;
  std::unique_ptr<Scheduler> scheduler_;
  std::unique_ptr<ArenaService> arena_;
  std::unique_ptr<FleetService> fleet_;
  std::unique_ptr<grpc::Server> server_;
  std::shared_ptr<grpc::Channel> channel_;
  std::unique_ptr<proto::Arena::Stub> arena_stub_;
  std::unique_ptr<proto::SandboxFleet::Stub> fleet_stub_;
  int port_ = 0;
};

TEST_F(ArenaIntegrationTest, SubmitReachesAWorkerAndComesBackRated) {
  grpc::ClientContext worker_context;
  auto worker = AttachWorker("w1", 2, &worker_context);
  // The worker is registered asynchronously by the Attach handler.
  for (int i = 0; i < 200 && scheduler_->worker_count() == 0; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  ASSERT_EQ(scheduler_->worker_count(), 1);

  const auto submitted = Submit("Alpha", "// alpha strategy\n");
  ASSERT_FALSE(submitted.candidate_id().empty());
  ASSERT_FALSE(submitted.job_id().empty());

  // The order arrives with everything a worker needs to reproduce the build.
  proto::FleetMessage message;
  ASSERT_TRUE(worker->Read(&message));
  ASSERT_TRUE(message.has_order());
  const proto::WorkOrder &order = message.order();
  EXPECT_EQ(order.candidate().candidate_id(), submitted.candidate_id());
  EXPECT_EQ(order.base_commit(), "deadbeef");
  EXPECT_EQ(order.opponent_spec(), "builtin:random");
  EXPECT_EQ(order.num_games(), 2);
  // The worker starts this itself, beside the bot, on a private network:
  // there is no broker address to hand out any more.
  EXPECT_EQ(order.referee_target(),
            "//game_arena/referee:match_referee");
  // The submission travels as a patch, generated BUILD included, so the worker
  // only ever runs `git apply`.
  EXPECT_NE(order.candidate().patch().find("+// alpha strategy"),
            std::string::npos)
      << order.candidate().patch();
  EXPECT_NE(order.candidate().patch().find("/BUILD"), std::string::npos);
  EXPECT_EQ(
      order.candidate().bot_target(),
      "//game_arena/candidates/" + submitted.candidate_id() + ":bot");

  ReportSuccess(worker.get(), order.order_id(), /*wins=*/2, /*losses=*/0,
                /*elo=*/1532.0);

  const proto::Job job = WaitForJob(submitted.job_id(), proto::Job::DONE);
  EXPECT_EQ(job.wins(), 2);
  EXPECT_EQ(job.games_played(), 2);
  EXPECT_EQ(job.candidate_id(), submitted.candidate_id());

  grpc::ClientContext get_context;
  proto::GetCandidateRequest get_request;
  get_request.set_candidate_id(submitted.candidate_id());
  proto::Candidate candidate;
  ASSERT_TRUE(
      arena_stub_->GetCandidate(&get_context, get_request, &candidate).ok());
  EXPECT_EQ(candidate.status(), proto::Candidate::READY);
  EXPECT_EQ(candidate.display_name(), "Alpha");

  worker_context.TryCancel();
}

// The whole premise of the arena: an agent can read any rival's source and
// build on it.
TEST_F(ArenaIntegrationTest, AnyAgentCanReadAnyCandidatesSource) {
  const auto submitted = Submit("Readable", "// the secret sauce\n");

  grpc::ClientContext context;
  proto::GetSourceRequest request;
  request.set_candidate_id(submitted.candidate_id());
  // Paths are repo-relative now: a submission is a patch, and a patch touches
  // repo paths. GetCandidate lists them, so an agent never has to guess.
  request.set_path("solutions/" + submitted.candidate_id() +
                   "/strategy.h");
  proto::SourceFile file;
  ASSERT_TRUE(arena_stub_->GetSource(&context, request, &file).ok());
  EXPECT_EQ(file.content(), "// the secret sauce\n");

  grpc::ClientContext missing_context;
  request.set_path("../../../etc/passwd");
  proto::SourceFile nothing;
  const grpc::Status status =
      arena_stub_->GetSource(&missing_context, request, &nothing);
  EXPECT_EQ(status.error_code(), grpc::StatusCode::NOT_FOUND);
}

TEST_F(ArenaIntegrationTest, RejectedSubmissionExplainsItself) {
  proto::SubmitRequest request;
  request.set_display_name("Sneaky");
  request.set_game("risk2");
  request.set_entry_header("strategy.h");
  auto *file = request.add_files();
  file->set_path("../../escape.h");
  file->set_content("// nope\n");

  grpc::ClientContext context;
  proto::SubmitResponse response;
  const grpc::Status status = arena_stub_->Submit(&context, request, &response);
  EXPECT_EQ(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
  EXPECT_NE(status.error_message().find(".."), std::string::npos);
  EXPECT_EQ(store_->size(), 0u);
}

TEST_F(ArenaIntegrationTest, BuildFailureSurfacesCompilerErrorsToTheAgent) {
  grpc::ClientContext worker_context;
  auto worker = AttachWorker("w1", 1, &worker_context);
  for (int i = 0; i < 200 && scheduler_->worker_count() == 0; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }

  const auto submitted = Submit("Broken");
  proto::FleetMessage message;
  ASSERT_TRUE(worker->Read(&message));
  ASSERT_TRUE(message.has_order());

  proto::WorkerMessage reply;
  auto *result = reply.mutable_result();
  result->set_order_id(message.order().order_id());
  result->set_build_ok(false);
  result->set_build_log(
      "strategy.h:7:10: error: 'MakePolicy' was not declared");
  ASSERT_TRUE(worker->Write(reply));

  const proto::Job job = WaitForJob(submitted.job_id(), proto::Job::FAILED);
  EXPECT_NE(job.error().find("was not declared"), std::string::npos);

  grpc::ClientContext get_context;
  proto::GetCandidateRequest get_request;
  get_request.set_candidate_id(submitted.candidate_id());
  proto::Candidate candidate;
  ASSERT_TRUE(
      arena_stub_->GetCandidate(&get_context, get_request, &candidate).ok());
  EXPECT_EQ(candidate.status(), proto::Candidate::BUILD_FAILED);
  EXPECT_NE(candidate.build_error().find("was not declared"),
            std::string::npos);

  worker_context.TryCancel();
}

TEST_F(ArenaIntegrationTest, LeaderboardRanksReadyCandidatesByElo) {
  const auto weak = Submit("Weak");
  const auto strong = Submit("Strong");
  ASSERT_TRUE(
      store_->SetStatus(weak.candidate_id(), proto::Candidate::READY, ""));
  ASSERT_TRUE(
      store_->SetStatus(strong.candidate_id(), proto::Candidate::READY, ""));
  elo_->RecordResult("risk2", strong.candidate_id(), weak.candidate_id(), 1.0);

  grpc::ClientContext context;
  proto::LeaderboardRequest request;
  request.set_game("risk2");
  proto::LeaderboardResponse response;
  ASSERT_TRUE(arena_stub_->Leaderboard(&context, request, &response).ok());
  ASSERT_EQ(response.rows_size(), 2);
  EXPECT_EQ(response.rows(0).candidate().candidate_id(), strong.candidate_id());
  EXPECT_GT(response.rows(0).score(), response.rows(1).score());
  EXPECT_EQ(response.rows(0).wins(), 1);
  EXPECT_EQ(response.rows(1).losses(), 1);
}

TEST_F(ArenaIntegrationTest, ListCandidatesFiltersAndOrders) {
  Submit("First");
  Submit("Second");

  grpc::ClientContext context;
  proto::ListCandidatesRequest request;
  request.set_game("risk2");
  request.set_order(proto::ListCandidatesRequest::NEWEST);
  proto::ListCandidatesResponse response;
  ASSERT_TRUE(arena_stub_->ListCandidates(&context, request, &response).ok());
  EXPECT_EQ(response.candidates_size(), 2);

  grpc::ClientContext other_context;
  request.set_author("nobody");
  proto::ListCandidatesResponse empty;
  ASSERT_TRUE(
      arena_stub_->ListCandidates(&other_context, request, &empty).ok());
  EXPECT_EQ(empty.candidates_size(), 0);
}

// A worker vanishing must not strand the job: its slot and its order come
// back, and the next worker picks the order up.
TEST_F(ArenaIntegrationTest, OrderIsRequeuedWhenAWorkerDisconnects) {
  const auto submitted = Submit("Resilient");

  {
    grpc::ClientContext first_context;
    auto worker = AttachWorker("w1", 1, &first_context);
    proto::FleetMessage message;
    ASSERT_TRUE(worker->Read(&message));
    ASSERT_TRUE(message.has_order());
    first_context.TryCancel();  // vanish mid-order
  }
  for (int i = 0; i < 400 && scheduler_->worker_count() != 0; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  ASSERT_EQ(scheduler_->worker_count(), 0);
  EXPECT_EQ(scheduler_->in_flight_orders(), 0);

  grpc::ClientContext second_context;
  auto replacement = AttachWorker("w2", 1, &second_context);
  proto::FleetMessage message;
  ASSERT_TRUE(replacement->Read(&message));
  ASSERT_TRUE(message.has_order());
  EXPECT_EQ(message.order().candidate().candidate_id(),
            submitted.candidate_id());

  ReportSuccess(replacement.get(), message.order().order_id(), 1, 1, 1500.0);
  WaitForJob(submitted.job_id(), proto::Job::DONE);
  second_context.TryCancel();
}

TEST_F(ArenaIntegrationTest, UnknownIdsAreNotFound) {
  grpc::ClientContext job_context;
  proto::GetJobRequest job_request;
  job_request.set_job_id("nope");
  proto::Job job;
  EXPECT_EQ(arena_stub_->GetJob(&job_context, job_request, &job).error_code(),
            grpc::StatusCode::NOT_FOUND);

  grpc::ClientContext candidate_context;
  proto::GetCandidateRequest candidate_request;
  candidate_request.set_candidate_id("nope");
  proto::Candidate candidate;
  EXPECT_EQ(
      arena_stub_
          ->GetCandidate(&candidate_context, candidate_request, &candidate)
          .error_code(),
      grpc::StatusCode::NOT_FOUND);

  grpc::ClientContext challenge_context;
  proto::EvaluateRequest evaluate;
  evaluate.set_candidate_id("nope");
  evaluate.mutable_match()->set_opponent("builtin:random");
  proto::EvaluateResponse response;
  EXPECT_EQ(arena_stub_->Evaluate(&challenge_context, evaluate, &response)
                .error_code(),
            grpc::StatusCode::INVALID_ARGUMENT);
}

// --- tokens and quotas ------------------------------------------------------
//
// Same wiring, but with a client registry, so writes are gated and metered.

class AuthenticatedArenaTest : public ArenaIntegrationTest {
 protected:
  static constexpr char kToken[] = "s3cret-token";

  auto MakeClients() -> std::unique_ptr<ClientRegistry> override {
    const auto path = dir_ / "clients.textproto";
    {
      std::ofstream out(path);
      out << "clients { client_id: \"agent-1\" token_sha256: \""
          << HashToken(kToken) << "\" }\n";
    }
    proto::ClientQuota defaults;
    defaults.set_max_active_evaluations(1);
    defaults.set_max_queued_jobs(4);
    auto registry = std::make_unique<ClientRegistry>(path, defaults);
    std::string error;
    EXPECT_TRUE(registry->Load(&error)) << error;
    return registry;
  }

  auto SubmitAs(const std::string &token, const std::string &name,
                bool cancel_running = false) -> grpc::Status {
    proto::SubmitRequest request;
    request.set_display_name(name);
    request.set_author("i-am-someone-else");
    request.set_game("risk2");
    request.set_entry_header("strategy.h");
    request.set_cancel_running(cancel_running);
    auto *file = request.add_files();
    file->set_path("strategy.h");
    file->set_content("// " + name + "\n");

    grpc::ClientContext context;
    if (!token.empty()) {
      context.AddMetadata("x-arena-token", token);
    }
    last_response_.Clear();
    return arena_stub_->Submit(&context, request, &last_response_);
  }

  proto::SubmitResponse last_response_;
};

TEST_F(AuthenticatedArenaTest, RefusesWritesWithoutAUsableToken) {
  EXPECT_EQ(SubmitAs("", "No Token").error_code(),
            grpc::StatusCode::UNAUTHENTICATED);
  EXPECT_EQ(SubmitAs("wrong-token", "Bad Token").error_code(),
            grpc::StatusCode::UNAUTHENTICATED);
  // Nothing was stored by either attempt.
  EXPECT_EQ(store_->size(), 0u);
}

// The leaderboard is public and readable source is the point of the arena.
TEST_F(AuthenticatedArenaTest, ReadsStayOpen) {
  ASSERT_TRUE(SubmitAs(kToken, "Readable").ok());

  grpc::ClientContext context;
  proto::LeaderboardResponse leaderboard;
  EXPECT_TRUE(
      arena_stub_
          ->Leaderboard(&context, proto::LeaderboardRequest{}, &leaderboard)
          .ok());

  grpc::ClientContext list_context;
  proto::ListCandidatesResponse listing;
  EXPECT_TRUE(arena_stub_
                  ->ListCandidates(&list_context,
                                   proto::ListCandidatesRequest{}, &listing)
                  .ok());
  EXPECT_EQ(listing.candidates_size(), 1);
}

// A quota you can enforce beside a credit you cannot is only half a system.
TEST_F(AuthenticatedArenaTest, AttributionComesFromTheTokenNotTheRequest) {
  ASSERT_TRUE(SubmitAs(kToken, "Mine").ok());
  const auto candidate = store_->Get(last_response_.candidate_id());
  ASSERT_TRUE(candidate.has_value());
  EXPECT_EQ(candidate->author(), "agent-1")
      << "the request claimed to be someone else";
}

TEST_F(AuthenticatedArenaTest, OneActiveEvaluationPerClient) {
  // A worker has to be attached for the first job to actually run: the limit is
  // on evaluations *running*, and queueing is bounded separately.
  grpc::ClientContext worker_context;
  auto worker = AttachWorker("w1", 4, &worker_context);
  ASSERT_TRUE(SubmitAs(kToken, "First").ok());
  const std::string first_job = last_response_.job_id();

  const grpc::Status second = SubmitAs(kToken, "Second");
  EXPECT_EQ(second.error_code(), grpc::StatusCode::RESOURCE_EXHAUSTED);
  EXPECT_NE(second.error_message().find(first_job), std::string::npos)
      << "the error should name what to wait for: " << second.error_message();
  // Refused means nothing stored, not stored-and-parked.
  EXPECT_EQ(store_->size(), 1u);
}

TEST_F(AuthenticatedArenaTest, CancelRunningTakesTheSlot) {
  grpc::ClientContext worker_context;
  auto worker = AttachWorker("w1", 4, &worker_context);
  ASSERT_TRUE(SubmitAs(kToken, "First").ok());
  const std::string first_job = last_response_.job_id();

  ASSERT_TRUE(SubmitAs(kToken, "Replacement", /*cancel_running=*/true).ok());
  const auto &superseded = last_response_.superseded_job_ids();
  EXPECT_NE(std::find(superseded.begin(), superseded.end(), first_job),
            superseded.end())
      << "the response should say what it replaced";

  // CANCELLED, not FAILED: the submitter replaced it, nothing broke.
  grpc::ClientContext context;
  proto::GetJobRequest request;
  request.set_job_id(first_job);
  proto::Job job;
  ASSERT_TRUE(arena_stub_->GetJob(&context, request, &job).ok());
  EXPECT_EQ(job.state(), proto::Job::CANCELLED);
  EXPECT_EQ(store_->size(), 2u);
  worker_context.TryCancel();
}

// Queued work is bounded separately from running work, so a client cannot bank
// a backlog behind its one active evaluation.
TEST_F(AuthenticatedArenaTest, BoundsQueuedWorkWithNoWorkerAttached) {
  // No worker, so nothing runs and everything queues. The registry allows 4.
  for (int i = 0; i < 4; ++i) {
    ASSERT_TRUE(SubmitAs(kToken, "Queued" + std::to_string(i)).ok()) << i;
  }
  const grpc::Status over = SubmitAs(kToken, "TooMany");
  EXPECT_EQ(over.error_code(), grpc::StatusCode::RESOURCE_EXHAUSTED);
  EXPECT_NE(over.error_message().find("queued"), std::string::npos)
      << over.error_message();
}

}  // namespace
}  // namespace tournament_arena
