// End-to-end test of SandboxRunnerService against a fake `docker`: a shell
// script that logs every invocation and emulates container behavior per
// container name ("*block*" containers run until killed, "*fail3*" exits 3).
// The overlay mount never actually happens, but the exact argv and entrypoint
// script handed to docker are asserted from the log.

#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <system_error>
#include <thread>
#include <utility>

#include <grpcpp/grpcpp.h>
#include <gtest/gtest.h>
#include <unistd.h>

#include "game_mcts/tournament_server/sandbox_runner/sandbox_runner.h"

namespace sandbox_runner {
namespace {

namespace proto = tournament_broker::proto;

class SandboxRunnerIntegrationTest : public ::testing::Test {
 protected:
  static void SetUpTestSuite() {
    root_ = std::filesystem::temp_directory_path() /
            ("sandbox_runner_itest_" + std::to_string(::getpid()));
    std::filesystem::create_directories(root_ / "repo");
    fake_docker_ = root_ / "fake_docker";
    std::ofstream(fake_docker_) << FakeDockerScript();
    std::filesystem::permissions(fake_docker_,
                                 std::filesystem::perms::owner_all,
                                 std::filesystem::perm_options::add);

    SandboxRunnerConfig config;
    config.docker = fake_docker_.string();
    config.docker_image = "fake-image:1";
    config.repo_dir = root_ / "repo";
    config.work_dir = root_ / "work";
    config.timeout = std::chrono::seconds(2);
    service_ = std::make_unique<SandboxRunnerService>(std::move(config));

    grpc::ServerBuilder builder;
    int port = 0;
    builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(),
                             &port);
    builder.RegisterService(service_.get());
    server_ = builder.BuildAndStart();
    stub_ = proto::SandboxService::NewStub(
        grpc::CreateChannel("127.0.0.1:" + std::to_string(port),
                            grpc::InsecureChannelCredentials()));
  }

  static void TearDownTestSuite() {
    server_->Shutdown();
    server_.reset();
    service_.reset();
    std::error_code ec;
    std::filesystem::remove_all(root_, ec);
  }

  static auto FakeDockerScript() -> std::string {
    return "#!/usr/bin/env bash\n"
           "echo \"docker $*\" >> \"" +
           (root_ / "docker.log").string() +
           "\"\n"
           "cmd=\"$1\"; shift || true\n"
           "case \"$cmd\" in\n"
           "  run)\n"
           "    name=\"\"\n"
           "    while [ $# -gt 0 ]; do\n"
           "      case \"$1\" in\n"
           "        --name) name=\"$2\"; shift 2;;\n"
           "        *) shift;;\n"
           "      esac\n"
           "    done\n"
           "    case \"$name\" in\n"
           "      *block*)\n"
           // bash execs a sole command, which would drop the marker from the
           // command line; the trailing `true` keeps bash alive so Kill's
           // pkill finds it.
           "        bash -c 'sleep 15; true' \"fake-sleeper-$name\"\n"
           "        exit 137;;\n"
           "      *fail3*)\n"
           "        echo \"fake stdout $name\"\n"
           "        echo \"fake stderr $name\" >&2\n"
           "        exit 3;;\n"
           "      *)\n"
           "        echo \"fake stdout $name\"\n"
           "        echo \"fake stderr $name\" >&2\n"
           "        exit 0;;\n"
           "    esac;;\n"
           "  kill)\n"
           "    pkill -f \"fake-sleeper-$1\"\n"
           "    exit 0;;\n"
           "esac\n"
           "exit 1\n";
  }

  static auto DockerLog() -> std::string {
    std::ifstream in(root_ / "docker.log");
    return {std::istreambuf_iterator<char>(in),
            std::istreambuf_iterator<char>()};
  }

  static void ExpectLogContains(const std::string &log,
                                const std::string &fragment) {
    EXPECT_NE(log.find(fragment), std::string::npos) << "fragment: " << fragment;
  }

  // Waits until docker has been invoked for |fragment| (or the deadline
  // passes), so tests can synchronize with a run in flight.
  static void WaitForLog(const std::string &fragment) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
      if (DockerLog().find(fragment) != std::string::npos) {
        return;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    ADD_FAILURE() << "timed out waiting for docker log fragment: " << fragment;
  }

  static auto Run(const proto::RunSandboxRequest &request,
                  proto::RunSandboxResponse *response) -> grpc::Status {
    grpc::ClientContext context;
    return stub_->Run(&context, request, response);
  }

  static auto Kill(const std::string &identifier) -> grpc::Status {
    grpc::ClientContext context;
    proto::KillRequest request;
    request.set_identifier(identifier);
    proto::KillResponse response;
    return stub_->Kill(&context, request, &response);
  }

  static std::filesystem::path root_;
  static std::filesystem::path fake_docker_;
  static std::unique_ptr<SandboxRunnerService> service_;
  static std::unique_ptr<grpc::Server> server_;
  static std::unique_ptr<proto::SandboxService::Stub> stub_;
};

std::filesystem::path SandboxRunnerIntegrationTest::root_;
std::filesystem::path SandboxRunnerIntegrationTest::fake_docker_;
std::unique_ptr<SandboxRunnerService> SandboxRunnerIntegrationTest::service_;
std::unique_ptr<grpc::Server> SandboxRunnerIntegrationTest::server_;
std::unique_ptr<proto::SandboxService::Stub> SandboxRunnerIntegrationTest::stub_;

TEST_F(SandboxRunnerIntegrationTest, RunCapturesOutputAndInvocation) {
  proto::RunSandboxRequest request;
  request.set_identifier("basic-1");
  request.set_bazel_target("//game_mcts/cpp:target");
  request.add_args("--flag=1");
  request.add_args("a b");
  (*request.mutable_patches())["game_mcts/cpp/new_file.cc"] = "int main() {}";

  proto::RunSandboxResponse response;
  const grpc::Status status = Run(request, &response);

  ASSERT_TRUE(status.ok()) << status.error_message();
  EXPECT_EQ(response.exit_code(), 0);
  EXPECT_EQ(response.stdout(), "fake stdout sbr-basic-1\n");
  EXPECT_EQ(response.stderr(), "fake stderr sbr-basic-1\n");

  // The docker invocation and the entrypoint script, as logged by the fake.
  const std::string log = DockerLog();
  ExpectLogContains(log, "--name sbr-basic-1");
  ExpectLogContains(log, "--cap-add SYS_ADMIN");
  ExpectLogContains(log, "/repo_lower:ro");
  ExpectLogContains(log, "/patches:ro");
  ExpectLogContains(log, "--entrypoint /bin/sh fake-image:1 -c");
  ExpectLogContains(log, "mount -t overlay overlay -o lowerdir=/repo_lower,");
  ExpectLogContains(log, "exec bazel run '//game_mcts/cpp:target' -- "
                         "'--flag=1' 'a b'");
}

TEST_F(SandboxRunnerIntegrationTest, RunPropagatesExitCode) {
  proto::RunSandboxRequest request;
  request.set_identifier("fail3-1");
  request.set_bazel_target("//broken:target");

  proto::RunSandboxResponse response;
  const grpc::Status status = Run(request, &response);

  ASSERT_TRUE(status.ok()) << status.error_message();
  EXPECT_EQ(response.exit_code(), 3);
}

TEST_F(SandboxRunnerIntegrationTest, RunValidatesRequests) {
  proto::RunSandboxResponse response;

  proto::RunSandboxRequest no_id;
  no_id.set_bazel_target("//x:y");
  EXPECT_EQ(Run(no_id, &response).error_code(),
            grpc::StatusCode::INVALID_ARGUMENT);

  proto::RunSandboxRequest escaping_patch;
  escaping_patch.set_identifier("bad-patch");
  escaping_patch.set_bazel_target("//x:y");
  (*escaping_patch.mutable_patches())["../outside.cc"] = "x";
  EXPECT_EQ(Run(escaping_patch, &response).error_code(),
            grpc::StatusCode::INVALID_ARGUMENT);
}

TEST_F(SandboxRunnerIntegrationTest, TimeoutKillsContainer) {
  proto::RunSandboxRequest request;
  request.set_identifier("block-timeout");
  request.set_bazel_target("//slow:target");

  proto::RunSandboxResponse response;
  const grpc::Status status = Run(request, &response);

  ASSERT_TRUE(status.ok()) << status.error_message();
  EXPECT_EQ(response.exit_code(), 124);
  EXPECT_NE(response.stderr().find("killed: timeout"), std::string::npos);
  // The server stopped the container by name after the client-side timeout.
  ExpectLogContains(DockerLog(), "docker kill sbr-block-timeout");
}

TEST_F(SandboxRunnerIntegrationTest, KillAbortsRunMidFlight) {
  proto::RunSandboxRequest request;
  request.set_identifier("block-kill");
  request.set_bazel_target("//slow:target");

  proto::RunSandboxResponse response;
  grpc::Status run_status;
  std::thread runner([&] { run_status = Run(request, &response); });
  WaitForLog("--name sbr-block-kill");

  ASSERT_TRUE(Kill("block-kill").ok());

  runner.join();
  ASSERT_TRUE(run_status.ok()) << run_status.error_message();
  EXPECT_EQ(response.exit_code(), 137);
}

TEST_F(SandboxRunnerIntegrationTest, KillUnknownIdentifierIsNotFound) {
  EXPECT_EQ(Kill("no-such-run").error_code(), grpc::StatusCode::NOT_FOUND);
}

TEST_F(SandboxRunnerIntegrationTest, DuplicateIdentifierIsRejected) {
  proto::RunSandboxRequest request;
  request.set_identifier("block-dup");
  request.set_bazel_target("//slow:target");

  proto::RunSandboxResponse background_response;
  std::thread runner(
      [&] { grpc::Status unused = Run(request, &background_response); });
  WaitForLog("--name sbr-block-dup");

  proto::RunSandboxResponse response;
  EXPECT_EQ(Run(request, &response).error_code(),
            grpc::StatusCode::ALREADY_EXISTS);

  ASSERT_TRUE(Kill("block-dup").ok());
  runner.join();
}

}  // namespace
}  // namespace sandbox_runner
