// The coordinator as it runs: the problem_server binary on a data dir of its
// own, restarted for real, with this test as the worker and the submitter
// over gRPC and as the reader of its pages over HTTP. What only main() wires
// (restart recovery, the matchmaker, the dashboard) is covered here. Manual
// and local: it binds ports and runs curl.
//
//   bazel test //game_arena/server:problem_server_it_test

#include <grpcpp/grpcpp.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>

#include "game_arena/common/sha256/sha256.h"
#include "game_arena/proto/arena.grpc.pb.h"
#include "game_arena/server/candidate_store.h"
#include "gtest/gtest.h"

namespace tournament_arena {
namespace {

using Stream =
    grpc::ClientReaderWriter<proto::WorkerMessage, proto::FleetMessage>;

// Nim, with every submission a version of its own and a pool kept playing.
constexpr char kConfig[] = R"pb(
  problem_id: "it"
  display_name: "integration"
  submission {
    allow_paths: "solutions/{submission_id}/**"
    files_submit_dir: "solutions"
    versions: true
    harness {
      api_dep: "//problem/harness:api"
      main_src: "//problem/harness:main.cc"
    }
  }
  build { targets: "//solutions/{submission_id}:bot" }
  sandbox { image: "it-image:1" }
  match {
    game: "nim"
    referee_target: "//game_arena/testgame:match_referee"
    games_per_order: 2
    placement_opponents: "builtin:random"
    matchmaking { pool: 10 games: 2 newcomer_games: 100 }
  }
  ranking { kind: TRUESKILL }
)pb";

int FreePort() {
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  ::bind(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address));
  socklen_t size = sizeof(address);
  ::getsockname(fd, reinterpret_cast<sockaddr *>(&address), &size);
  ::close(fd);
  return ntohs(address.sin_port);
}

class ProblemServerTest : public ::testing::Test {
 protected:
  void SetUp() override {
    dir_ = std::filesystem::path(std::getenv("TEST_TMPDIR")) /
           ::testing::UnitTest::GetInstance()->current_test_info()->name();
    std::filesystem::create_directories(dir_ / "data");
    std::ofstream(dir_ / "problem.textproto") << kConfig;
    grpc_port_ = FreePort();
    http_port_ = FreePort();
    Start();
  }

  void TearDown() override { Stop(); }

  void Start() {
    const std::string binary = std::string(std::getenv("TEST_SRCDIR")) +
                               "/_main/game_arena/server/problem_server";
    pid_ = ::fork();
    if (pid_ == 0) {
      const std::string log = (dir_ / "server.log").string();
      std::freopen(log.c_str(), "a", stderr);
      std::freopen(log.c_str(), "a", stdout);
      const std::string config =
          "--problem_config=" + (dir_ / "problem.textproto").string();
      const std::string data = "--data_dir=" + (dir_ / "data").string();
      const std::string grpc = "--grpc_port=" + std::to_string(grpc_port_);
      const std::string http = "--http_port=" + std::to_string(http_port_);
      ::execl(binary.c_str(), binary.c_str(), config.c_str(), data.c_str(),
              grpc.c_str(), http.c_str(), "--shutdown_grace_s=1", nullptr);
      std::_Exit(127);
    }
    for (int i = 0; i < 300 && Get("/api/candidates").empty(); ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    ASSERT_NE(Get("/api/candidates"), "") << Log();
    channel_ = grpc::CreateChannel("127.0.0.1:" + std::to_string(grpc_port_),
                                   grpc::InsecureChannelCredentials());
    arena_ = proto::Arena::NewStub(channel_);
    fleet_ = proto::SandboxFleet::NewStub(channel_);
  }

  void Stop() {
    if (pid_ > 0) {
      ::kill(pid_, SIGTERM);
      ::waitpid(pid_, nullptr, 0);
      pid_ = 0;
    }
  }

  std::string Log() const {
    std::ifstream in(dir_ / "server.log");
    return {std::istreambuf_iterator<char>(in),
            std::istreambuf_iterator<char>()};
  }

  // The body of an HTTP GET, or empty.
  std::string Get(const std::string &path) const {
    std::string body;
    FILE *pipe = ::popen(
        ("curl -sf http://127.0.0.1:" + std::to_string(http_port_) + path)
            .c_str(),
        "r");
    std::array<char, 4096> buffer;
    while (std::fgets(buffer.data(), buffer.size(), pipe) != nullptr) {
      body += buffer.data();
    }
    ::pclose(pipe);
    return body;
  }

  proto::SubmitResponse Submit(const std::string &name,
                               const std::string &content) {
    proto::SubmitRequest request;
    request.set_display_name(name);
    request.set_game("nim");
    request.set_entry_header("strategy.h");
    auto *file = request.add_files();
    file->set_path("strategy.h");
    file->set_content(content);
    grpc::ClientContext context;
    proto::SubmitResponse response;
    const grpc::Status status = arena_->Submit(&context, request, &response);
    EXPECT_TRUE(status.ok()) << status.error_message();
    return response;
  }

  proto::Candidate Candidate(const std::string &id) {
    grpc::ClientContext context;
    proto::GetCandidateRequest request;
    request.set_candidate_id(id);
    proto::Candidate candidate;
    EXPECT_TRUE(arena_->GetCandidate(&context, request, &candidate).ok()) << id;
    return candidate;
  }

  proto::Job Job(const std::string &id) {
    grpc::ClientContext context;
    proto::GetJobRequest request;
    request.set_job_id(id);
    proto::Job job;
    EXPECT_TRUE(arena_->GetJob(&context, request, &job).ok()) << id;
    return job;
  }

  std::unique_ptr<Stream> Attach(int slots, grpc::ClientContext *context) {
    std::unique_ptr<Stream> stream = fleet_->Attach(context);
    proto::WorkerMessage hello;
    hello.mutable_hello()->set_worker_id("it-worker");
    hello.mutable_hello()->set_slots(slots);
    hello.mutable_hello()->set_builds_artifacts(true);
    EXPECT_TRUE(stream->Write(hello));
    return stream;
  }

  static proto::WorkOrder NextOrder(Stream *stream) {
    proto::FleetMessage message;
    while (stream->Read(&message) && !message.has_order()) {
    }
    return message.order();
  }

  static void Answer(Stream *stream, const proto::OrderResult &result) {
    proto::WorkerMessage message;
    *message.mutable_result() = result;
    EXPECT_TRUE(stream->Write(message));
  }

  // Uploads |bytes| to the coordinator's archive, as a worker does.
  std::string Archive(const std::string &bytes) {
    grpc::ClientContext context;
    proto::PutArtifactResponse response;
    const auto writer = fleet_->PutArtifact(&context, &response);
    proto::ArtifactChunk chunk;
    chunk.set_digest(sha256::Hex(bytes));
    chunk.set_data(bytes);
    EXPECT_TRUE(writer->Write(chunk));
    writer->WritesDone();
    EXPECT_TRUE(writer->Finish().ok());
    return response.digest();
  }

  std::string Fetch(const std::string &digest) {
    grpc::ClientContext context;
    proto::GetArtifactRequest request;
    request.set_digest(digest);
    const auto reader = fleet_->GetArtifact(&context, request);
    std::string bytes;
    proto::ArtifactChunk chunk;
    while (reader->Read(&chunk)) {
      bytes += chunk.data();
    }
    return reader->Finish().ok() ? bytes : "";
  }

  // Answers a build order as a worker would: the binaries into the archive.
  proto::OrderResult Built(const proto::WorkOrder &order) {
    EXPECT_TRUE(order.build_only()) << order.DebugString();
    proto::OrderResult result;
    result.set_order_id(order.order_id());
    result.set_build_ok(true);
    (*result.mutable_artifacts())[order.candidate().bot_target()] =
        Archive("bot " + order.candidate().candidate_id());
    if (!order.referee_target().empty()) {
      (*result.mutable_artifacts())[order.referee_target()] =
          Archive("the referee");
    }
    return result;
  }

  static proto::OrderResult Played(const proto::WorkOrder &order) {
    proto::OrderResult result;
    result.set_order_id(order.order_id());
    result.set_build_ok(true);
    result.set_games_played(2);
    result.set_wins(1);
    result.set_losses(1);
    return result;
  }

  template <typename Done>
  static bool Eventually(Done done) {
    for (int i = 0; i < 200; ++i) {
      if (done()) {
        return true;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return false;
  }

  std::filesystem::path dir_;
  int grpc_port_ = 0;
  int http_port_ = 0;
  pid_t pid_ = 0;
  std::shared_ptr<grpc::Channel> channel_;
  std::unique_ptr<proto::Arena::Stub> arena_;
  std::unique_ptr<proto::SandboxFleet::Stub> fleet_;
};

TEST_F(ProblemServerTest, ARestartClosesOpenJobsAndPlacesPendingAgain) {
  // No worker: the placement is still queued when the coordinator goes down.
  const proto::SubmitResponse submitted = Submit("alice", "// alice\n");
  ASSERT_EQ(submitted.candidate_id(), "alice-v01");
  Stop();
  Start();

  const proto::Job before = Job(submitted.job_id());
  EXPECT_EQ(before.state(), proto::Job::CANCELLED);
  EXPECT_EQ(before.error(), "interrupted by a restart");
  EXPECT_EQ(Candidate("alice-v01").status(), proto::Candidate::PENDING);

  // Placed again: built, then played, by the first worker to attach.
  grpc::ClientContext context;
  const std::unique_ptr<Stream> worker = Attach(1, &context);
  const proto::WorkOrder build = NextOrder(worker.get());
  EXPECT_EQ(build.candidate().candidate_id(), "alice-v01");
  Answer(worker.get(), Built(build));
  const proto::WorkOrder order = NextOrder(worker.get());
  EXPECT_FALSE(order.build_only());
  Answer(worker.get(), Played(order));
  EXPECT_TRUE(Eventually([&] {
    return Candidate("alice-v01").status() == proto::Candidate::READY;
  }));
  context.TryCancel();
}

TEST_F(ProblemServerTest, AnEngineErrorRetriesTheBuildAndBlamesNoOne) {
  grpc::ClientContext context;
  const std::unique_ptr<Stream> worker = Attach(1, &context);

  Submit("alice", "// alice\n");
  proto::WorkOrder order = NextOrder(worker.get());
  proto::OrderResult result;
  result.set_order_id(order.order_id());
  result.set_error(
      "cannot create the phase network: Error response from daemon: all "
      "predefined address pools have been fully subnetted");
  Answer(worker.get(), result);

  // The same build again, and alice's code not blamed.
  order = NextOrder(worker.get());
  EXPECT_TRUE(order.build_only());
  EXPECT_EQ(order.candidate().candidate_id(), "alice-v01");
  EXPECT_EQ(Candidate("alice-v01").status(), proto::Candidate::PENDING);

  // A broken build names whose it was; that one is failed.
  result.Clear();
  result.set_order_id(order.order_id());
  result.set_build_failed_candidate_id("alice-v01");
  result.set_build_log("strategy.h:1:1: error: expected unqualified-id");
  Answer(worker.get(), result);
  EXPECT_TRUE(Eventually([&] {
    return Candidate("alice-v01").status() == proto::Candidate::BUILD_FAILED;
  }));
  context.TryCancel();
}

TEST_F(ProblemServerTest, MatchPagesShowTheCodeAndANameItsNewestVersion) {
  grpc::ClientContext context;
  const std::unique_ptr<Stream> worker = Attach(1, &context);
  for (const char *code : {"// alice one\n", "// alice two\n"}) {
    Submit("alice", code);
    Answer(worker.get(), Built(NextOrder(worker.get())));
    Answer(worker.get(), Played(NextOrder(worker.get())));
  }

  // Both READY and nothing queued: the matchmaker pairs them, from the
  // archive: nothing in the order is built again.
  const proto::WorkOrder match = NextOrder(worker.get());
  ASSERT_TRUE(match.has_opponent()) << match.DebugString();
  EXPECT_FALSE(match.build_only());
  EXPECT_EQ(Fetch(match.candidate().artifact()),
            "bot " + match.candidate().candidate_id());
  EXPECT_EQ(Fetch(match.opponent().artifact()),
            "bot " + match.opponent().candidate_id());
  EXPECT_EQ(Fetch(match.referee_artifact()), "the referee");
  Answer(worker.get(), Played(match));

  // A match records no patch; its page shows the versions' stored code.
  std::string match_job;
  EXPECT_TRUE(Eventually([&] {
    const std::string pool = Get("/pool");
    const auto at = pool.find("/jobs/j");
    match_job =
        at == std::string::npos ? "" : pool.substr(at, pool.find('"', at) - at);
    return !match_job.empty();
  })) << Get("/pool");
  EXPECT_NE(Get(match_job).find("// alice"), std::string::npos)
      << Get(match_job);

  const std::string alice = Get("/participants/alice");
  EXPECT_NE(alice.find("// alice two"), std::string::npos) << alice;
  EXPECT_NE(alice.find("solutions/alice-v02/strategy.h"), std::string::npos);
  context.TryCancel();
}

// A season from before the archive: every READY version is built into it
// once at startup, plays nothing, and is not built again after a restart.
TEST_F(ProblemServerTest, ReadyVersionsFromBeforeTheArchiveAreBackfilled) {
  Stop();
  {
    proto::SubmissionPolicy rules;
    rules.set_files_submit_dir("solutions");
    rules.add_allow_paths("solutions/{submission_id}/**");
    rules.set_versions(true);
    rules.mutable_harness()->set_api_dep("//problem/harness:api");
    rules.mutable_harness()->set_main_src("//problem/harness:main.cc");
    CandidateStore old_season(dir_ / "data" / "candidates", CandidateLimits{},
                              rules);
    proto::SubmitRequest request;
    request.set_display_name("carol");
    request.set_game("nim");
    request.set_entry_header("strategy.h");
    auto *file = request.add_files();
    file->set_path("strategy.h");
    file->set_content("// carol\n");
    std::string error;
    ASSERT_TRUE(old_season.Create(request, &error).has_value()) << error;
    old_season.SetStatus("carol-v01", proto::Candidate::READY, "");
  }
  Start();

  grpc::ClientContext context;
  std::unique_ptr<Stream> worker = Attach(1, &context);
  const proto::WorkOrder build = NextOrder(worker.get());
  EXPECT_TRUE(build.build_only());
  EXPECT_EQ(build.candidate().candidate_id(), "carol-v01");
  Answer(worker.get(), Built(build));
  EXPECT_TRUE(
      Eventually([&] { return !Candidate("carol-v01").artifact().empty(); }));
  EXPECT_EQ(Candidate("carol-v01").status(), proto::Candidate::READY);
  const std::string digest = Candidate("carol-v01").artifact();
  context.TryCancel();

  Stop();
  Start();
  EXPECT_EQ(Candidate("carol-v01").artifact(), digest);
  EXPECT_EQ(Fetch(digest), "bot carol-v01");
  EXPECT_EQ(Get("/api/candidates").find("\"PENDING\""), std::string::npos);
}

}  // namespace
}  // namespace tournament_arena
