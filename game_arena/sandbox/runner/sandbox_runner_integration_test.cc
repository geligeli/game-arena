// End-to-end test of SandboxRunnerService against a fake `docker`: a shell
// script that logs every invocation and emulates container behavior per
// container name ("*block*" containers run until killed, "*fail3*" exits 3).
// The overlay mount never actually happens, but the exact argv and entrypoint
// script handed to docker are asserted from the log.

#include <grpcpp/grpcpp.h>
#include <gtest/gtest.h>
#include <unistd.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "game_arena/sandbox/exec/container_engine.h"
#include "game_arena/sandbox/runner/sandbox_runner.h"

namespace sandbox_runner {
namespace {

namespace proto = sandbox_runner::proto;
namespace sx = sandbox_exec::proto;

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
    config.docker_image = "fake-image:1";
    config.repo_dir = root_ / "repo";
    config.work_dir = root_ / "work";
    config.timeout = std::chrono::seconds(2);
    sandbox_exec::ContainerEngineConfig engine_config;
    engine_config.docker = fake_docker_.string();
    engine_ = std::make_unique<sandbox_exec::ContainerEngine>(engine_config);
    service_ = std::make_unique<SandboxRunnerService>(std::move(config),
                                                      engine_.get());

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
           "    name=\"\"; patchsrc=\"\"\n"
           "    while [ $# -gt 0 ]; do\n"
           "      case \"$1\" in\n"
           "        --name) name=\"$2\"; shift 2;;\n"
           // The daemon resolves bind sources itself, so the fake reports
           // what it finds at the one docker was pointed at: that is the
           // difference between a mounted patch set and an empty directory.
           "        --mount)\n"
           "          case \"$2\" in\n"
           "            *target=/patches,*) patchsrc=\"${2#*source=}\"\n"
           "              patchsrc=\"${patchsrc%%,*}\";;\n"
           "          esac\n"
           "          shift 2;;\n"
           "        *) shift;;\n"
           "      esac\n"
           "    done\n"
           "    if [ -d \"$patchsrc\" ]; then\n"
           "      echo \"patchsrc $(ls \"$patchsrc\" | tr '\\n' ' ')\" >> \"" +
           (root_ / "docker.log").string() +
           "\"\n"
           "    else\n"
           "      echo \"patchsrc MISSING\" >> \"" +
           (root_ / "docker.log").string() +
           "\"\n"
           "    fi\n"
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
    EXPECT_NE(log.find(fragment), std::string::npos)
        << "fragment: " << fragment;
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

  // `bazel run <target> -- <args...>`, which is what this tool has always
  // sent; the words that are not data go through unquoted.
  static void SetCommand(proto::RunRequest *request, const std::string &target,
                         const std::vector<std::string> &args) {
    const auto add = [request](const std::string &text, bool verbatim) {
      sx::Token *token = request->add_argv();
      token->set_text(text);
      token->set_verbatim(verbatim);
    };
    add("bazel", true);
    add("run", true);
    add(target, false);
    if (!args.empty()) {
      add("--", true);
      for (const std::string &arg : args) {
        add(arg, false);
      }
    }
  }

  static auto Run(const proto::RunRequest &request,
                  proto::RunResponse *response) -> grpc::Status {
    grpc::ClientContext context;
    return stub_->Run(&context, request, response);
  }

  static auto Kill(const std::string &identifier) -> grpc::Status {
    grpc::ClientContext context;
    proto::KillRequest request;
    request.set_id(identifier);
    proto::KillResponse response;
    return stub_->Kill(&context, request, &response);
  }

  static std::filesystem::path root_;
  static std::filesystem::path fake_docker_;
  static std::unique_ptr<sandbox_exec::ContainerEngine> engine_;
  static std::unique_ptr<SandboxRunnerService> service_;
  static std::unique_ptr<grpc::Server> server_;
  static std::unique_ptr<proto::SandboxService::Stub> stub_;
};

std::filesystem::path SandboxRunnerIntegrationTest::root_;
std::filesystem::path SandboxRunnerIntegrationTest::fake_docker_;
std::unique_ptr<sandbox_exec::ContainerEngine>
    SandboxRunnerIntegrationTest::engine_;
std::unique_ptr<SandboxRunnerService> SandboxRunnerIntegrationTest::service_;
std::unique_ptr<grpc::Server> SandboxRunnerIntegrationTest::server_;
std::unique_ptr<proto::SandboxService::Stub>
    SandboxRunnerIntegrationTest::stub_;

TEST_F(SandboxRunnerIntegrationTest, RunCapturesOutputAndInvocation) {
  proto::RunRequest request;
  request.set_id("basic-1");
  SetCommand(&request, "//problem/app:target", {"--flag=1", "a b"});
  sx::StagedFile *f_problem_app_new_file_cc = request.add_files();
  f_problem_app_new_file_cc->set_path("problem/app/new_file.cc");
  f_problem_app_new_file_cc->set_content("int main() {}");

  proto::RunResponse response;
  const grpc::Status status = Run(request, &response);

  ASSERT_TRUE(status.ok()) << status.error_message();
  EXPECT_EQ(response.exit_code(), 0);
  EXPECT_EQ(response.stdout(), "fake stdout sbr-basic-1\n");
  EXPECT_EQ(response.stderr(), "fake stderr sbr-basic-1\n");

  // The docker invocation and the entrypoint script, as logged by the fake.
  const std::string log = DockerLog();
  ExpectLogContains(log, "--name sbr-basic-1");
  ExpectLogContains(log, "--cap-add SYS_ADMIN");
  ExpectLogContains(log,
                    "--mount type=bind,source=" + (root_ / "repo").string() +
                        ",target=/repo_lower,readonly");
  ExpectLogContains(log,
                    "--mount type=bind,source=" +
                        (root_ / "work" / "sbr-basic-1" / "patches").string() +
                        ",target=/patches,readonly");
  // Docker was pointed at a directory that really holds the patches, not one
  // it had to conjure up.
  ExpectLogContains(log, "patchsrc problem");
  ExpectLogContains(log, "--entrypoint /bin/sh fake-image:1 -c");
  ExpectLogContains(log, "mount -t overlay overlay -o lowerdir=/repo_lower,");
  ExpectLogContains(log,
                    "exec bazel run '//problem/app:target' -- "
                    "'--flag=1' 'a b'");
}

// Docker-outside-of-docker: the daemon resolves bind sources in a tree this
// process cannot see, so the host paths must reach docker's argv -- and only
// docker's argv. The patches themselves are still written under work_dir,
// where this process can write them.
TEST_F(SandboxRunnerIntegrationTest, HostPathsRedirectOnlyTheBindMounts) {
  SandboxRunnerConfig config;
  config.docker_image = "fake-image:1";
  config.repo_dir = root_ / "repo";
  config.work_dir = root_ / "work";
  config.host_repo_dir = "/host/view/repo";
  config.host_work_dir = "/host/view/work";
  config.timeout = std::chrono::seconds(2);
  // Called directly: the RPC handler ignores its ServerContext, and a second
  // service on the shared stub would need a second server.
  sandbox_exec::ContainerEngineConfig local_engine_config;
  local_engine_config.docker = fake_docker_.string();
  sandbox_exec::ContainerEngine local_engine(local_engine_config);
  SandboxRunnerService service(std::move(config), &local_engine);

  proto::RunRequest request;
  request.set_id("hostpaths-1");
  SetCommand(&request, "//x:y", {});
  sx::StagedFile *f_only_cc = request.add_files();
  f_only_cc->set_path("only.cc");
  f_only_cc->set_content("int only;");
  proto::RunResponse response;
  const grpc::Status status = service.Run(nullptr, &request, &response);

  ASSERT_TRUE(status.ok()) << status.error_message();
  const std::string log = DockerLog();
  ExpectLogContains(
      log,
      "--mount type=bind,source=/host/view/repo,target=/repo_lower,"
      "readonly");
  ExpectLogContains(
      log,
      "--mount type=bind,source=/host/view/work/sbr-hostpaths-1/patches,"
      "target=/patches,readonly");
  // The overlay's lower dir is a fixed mount point, so the host path never
  // leaks into the in-container script.
  ExpectLogContains(log, "mount -t overlay overlay -o lowerdir=/repo_lower,");
  // The fake docker resolves that source the way a real daemon would -- in
  // its own filesystem, where the host path is absent. That asymmetry is
  // precisely why main.cc cannot verify a --host_* path locally.
  ExpectLogContains(log, "patchsrc MISSING");
}

TEST_F(SandboxRunnerIntegrationTest, RunPropagatesExitCode) {
  proto::RunRequest request;
  request.set_id("fail3-1");
  SetCommand(&request, "//broken:target", {});

  proto::RunResponse response;
  const grpc::Status status = Run(request, &response);

  ASSERT_TRUE(status.ok()) << status.error_message();
  EXPECT_EQ(response.exit_code(), 3);
}

TEST_F(SandboxRunnerIntegrationTest, RunValidatesRequests) {
  proto::RunResponse response;

  proto::RunRequest no_id;
  SetCommand(&no_id, "//x:y", {});
  EXPECT_EQ(Run(no_id, &response).error_code(),
            grpc::StatusCode::INVALID_ARGUMENT);

  proto::RunRequest escaping_patch;
  escaping_patch.set_id("bad-patch");
  SetCommand(&escaping_patch, "//x:y", {});
  sx::StagedFile *f____outside_cc = escaping_patch.add_files();
  f____outside_cc->set_path("../outside.cc");
  f____outside_cc->set_content("x");
  EXPECT_EQ(Run(escaping_patch, &response).error_code(),
            grpc::StatusCode::INVALID_ARGUMENT);
}

TEST_F(SandboxRunnerIntegrationTest, TimeoutKillsContainer) {
  proto::RunRequest request;
  request.set_id("block-timeout");
  SetCommand(&request, "//slow:target", {});

  proto::RunResponse response;
  const grpc::Status status = Run(request, &response);

  ASSERT_TRUE(status.ok()) << status.error_message();
  EXPECT_EQ(response.exit_code(), 124);
  EXPECT_NE(response.stderr().find("killed: timeout"), std::string::npos);
  // The server stopped the container by name after the client-side timeout.
  ExpectLogContains(DockerLog(), "docker kill sbr-block-timeout");
}

TEST_F(SandboxRunnerIntegrationTest, KillAbortsRunMidFlight) {
  proto::RunRequest request;
  request.set_id("block-kill");
  SetCommand(&request, "//slow:target", {});

  proto::RunResponse response;
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
  proto::RunRequest request;
  request.set_id("block-dup");
  SetCommand(&request, "//slow:target", {});

  proto::RunResponse background_response;
  std::thread runner(
      [&] { grpc::Status unused = Run(request, &background_response); });
  WaitForLog("--name sbr-block-dup");

  proto::RunResponse response;
  EXPECT_EQ(Run(request, &response).error_code(),
            grpc::StatusCode::ALREADY_EXISTS);

  ASSERT_TRUE(Kill("block-dup").ok());
  runner.join();
}

}  // namespace
}  // namespace sandbox_runner
