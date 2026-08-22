// End-to-end through the arena's control plane over real gRPC: submit a
// candidate, watch a worker receive the order, report a result, and see the
// standings move.
//
// The worker here is a stub that fakes the build and the games. That is the
// point: everything between "an agent submits" and "a result comes back" is
// exercised without a git clone, a bazel build, or a real match, so this test
// runs in milliseconds and fails for reasons in this repo's control plane
// rather than in a toolchain.

#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>

#include <grpcpp/grpcpp.h>

#include "gtest/gtest.h"
#include "game_mcts/tournament_server/arena.grpc.pb.h"
#include "game_mcts/tournament_server/arena_service.h"
#include "game_mcts/tournament_server/candidate_store.h"
#include "game_mcts/tournament_server/elo_store.h"
#include "game_mcts/tournament_server/fleet_service.h"
#include "game_mcts/tournament_server/scheduler.h"

#include <unistd.h>

namespace tournament_arena {
namespace {

using Stream =
    grpc::ClientReaderWriter<proto::WorkerMessage, proto::FleetMessage>;

class ArenaIntegrationTest : public ::testing::Test {
 protected:
  void SetUp() override {
    dir_ = std::filesystem::temp_directory_path() /
           ("arena_it_" + std::to_string(::getpid()) + "_" +
            std::to_string(
                ::testing::UnitTest::GetInstance()->random_seed()));
    std::filesystem::remove_all(dir_);

    store_ = std::make_unique<CandidateStore>(dir_ / "candidates");
    elo_ = std::make_unique<tournament_broker::EloStore>(dir_ / "ratings.pb",
                                                         32.0);
    SchedulerConfig config;
    config.placement_opponents = {"builtin:random"};
    config.placement_games = 2;
    config.broker_target = "127.0.0.1:50051";
    scheduler_ = std::make_unique<Scheduler>(config, store_.get(), elo_.get());
    arena_ = std::make_unique<ArenaService>(store_.get(), scheduler_.get(),
                                            elo_.get(), "deadbeef");
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

  auto WaitForJob(const std::string &job_id, proto::Job::State state)
      -> proto::Job {
    proto::Job job;
    for (int i = 0; i < 400; ++i) {
      grpc::ClientContext context;
      proto::GetJobRequest request;
      request.set_job_id(job_id);
      const grpc::Status status =
          arena_stub_->GetJob(&context, request, &job);
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
  EXPECT_EQ(order.candidate_id(), submitted.candidate_id());
  EXPECT_EQ(order.base_commit(), "deadbeef");
  EXPECT_EQ(order.opponent(), "builtin:random");
  EXPECT_EQ(order.num_games(), 2);
  EXPECT_EQ(order.broker_target(), "127.0.0.1:50051");
  EXPECT_EQ(order.entry_header(), "strategy.h");
  ASSERT_EQ(order.files_size(), 1);
  EXPECT_EQ(order.files(0).content(), "// alpha strategy\n");

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
  request.set_path("strategy.h");
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
  const grpc::Status status =
      arena_stub_->Submit(&context, request, &response);
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
  result->set_build_log("strategy.h:7:10: error: 'MakePolicy' was not declared");
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
  ASSERT_TRUE(store_->SetStatus(weak.candidate_id(), proto::Candidate::READY,
                                ""));
  ASSERT_TRUE(store_->SetStatus(strong.candidate_id(),
                                proto::Candidate::READY, ""));
  elo_->RecordResult("risk2", strong.candidate_id(), weak.candidate_id(), 1.0);

  grpc::ClientContext context;
  proto::LeaderboardRequest request;
  request.set_game("risk2");
  proto::LeaderboardResponse response;
  ASSERT_TRUE(arena_stub_->Leaderboard(&context, request, &response).ok());
  ASSERT_EQ(response.rows_size(), 2);
  EXPECT_EQ(response.rows(0).candidate().candidate_id(),
            strong.candidate_id());
  EXPECT_GT(response.rows(0).elo(), response.rows(1).elo());
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
  ASSERT_TRUE(
      arena_stub_->ListCandidates(&context, request, &response).ok());
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
  EXPECT_EQ(message.order().candidate_id(), submitted.candidate_id());

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
  EXPECT_EQ(arena_stub_
                ->GetCandidate(&candidate_context, candidate_request,
                               &candidate)
                .error_code(),
            grpc::StatusCode::NOT_FOUND);

  grpc::ClientContext challenge_context;
  proto::ChallengeRequest challenge;
  challenge.set_candidate_id("nope");
  challenge.set_opponent("builtin:random");
  proto::ChallengeResponse response;
  EXPECT_EQ(arena_stub_
                ->Challenge(&challenge_context, challenge, &response)
                .error_code(),
            grpc::StatusCode::INVALID_ARGUMENT);
}

}  // namespace
}  // namespace tournament_arena
