// End-to-end test of DockerBackend against fake `docker` and `git` scripts:
// shell scripts that log every invocation and emulate just enough of each
// tool (a clone that creates a .git dir, a checkout that records its commit,
// containers that behave per container name). The overlay mount never
// actually happens, but the exact mounts, argv and entrypoint scripts handed
// to docker, and the host-side git work, are asserted from the logs.

#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <system_error>

#include <gtest/gtest.h>
#include <unistd.h>

#include "game_mcts/tournament_server/sandbox/worker/docker_backend.h"

namespace tournament_arena {
namespace {

namespace proto = tournament_arena::proto;

constexpr char kFakeCommit[] = "abc123def456";

class DockerBackendIntegrationTest : public ::testing::Test {
 protected:
  static void SetUpTestSuite() {
    root_ = std::filesystem::temp_directory_path() /
            ("docker_backend_itest_" + std::to_string(::getpid()));
    // A "repository" for the backend to clone: the fake git only needs the
    // directory to exist, but Warmup requires the .git marker.
    std::filesystem::create_directories(root_ / "repo_src" / ".git");

    fake_docker_ = root_ / "fake_docker";
    std::ofstream(fake_docker_) << FakeDockerScript();
    std::filesystem::permissions(fake_docker_,
                                 std::filesystem::perms::owner_all,
                                 std::filesystem::perm_options::add);
    fake_git_ = root_ / "fake_git";
    std::ofstream(fake_git_) << FakeGitScript();
    std::filesystem::permissions(fake_git_,
                                 std::filesystem::perms::owner_all,
                                 std::filesystem::perm_options::add);

    DockerBackendConfig config;
    config.docker = fake_docker_.string();
    config.git = fake_git_.string();
    config.docker_image = "fake-image:1";
    config.repo_dir = root_ / "repo_src";
    config.work_dir = root_ / "work";
    config.disk_cache = root_ / "work" / "disk_cache";
    backend_ = std::make_unique<DockerBackend>(std::move(config));

    std::string error;
    ASSERT_TRUE(backend_->Warmup(2, &error)) << error;
  }

  static void TearDownTestSuite() {
    backend_.reset();
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
           "    name=\"\"; prev=\"\"\n"
           "    for a in \"$@\"; do\n"
           "      if [ \"$prev\" = \"--name\" ]; then name=\"$a\"; fi\n"
           "      prev=\"$a\"\n"
           "    done\n"
           "    case \"$name\" in\n"
           // A candidate whose build fails: the container prints compiler
           // diagnostics and a nonzero exit, like bazel would.
           "      *failbuild-1-build*)\n"
           "        echo \"game_mcts/tournament_server/candidates/failbuild-1/strategy.h:3:5: error: expected ';' before '}' token\"\n"
           "        echo \"Target //game_mcts/tournament_server/candidates/failbuild-1:bot failed to build\"\n"
           "        exit 1;;\n"
           // A bot that never finishes: runs until the server-side timeout
           // kills the container. bash execs a sole command, which would drop
           // the marker from the command line; the trailing `true` keeps
           // bash alive so Kill's pkill finds it.
           "      *blockrun-1-run*)\n"
           "        bash -c 'sleep 15; true' \"fake-sleeper-$name\"\n"
           "        exit 137;;\n"
           // The bot: prints the RESULT line the worker parses.
           "      *-run)\n"
           "        echo \"RESULT games=2 wins=1 draws=1 losses=0 elo=1500.0\"\n"
           "        exit 0;;\n"
           // A clean build.
           "      *-build)\n"
           "        echo \"INFO: Build completed successfully, 42 total actions\"\n"
           "        exit 0;;\n"
           "    esac;;\n"
           "  rm)\n"
           "    exit 0;;\n"
           "  kill)\n"
           "    pkill -f \"fake-sleeper-$1\"\n"
           "    exit 0;;\n"
           "esac\n"
           "exit 1\n";
  }

  static auto FakeGitScript() -> std::string {
    return "#!/usr/bin/env bash\n"
           "echo \"git $*\" >> \"" +
           (root_ / "git.log").string() +
           "\"\n"
           "cmd=\"$1\"; shift || true\n"
           "case \"$cmd\" in\n"
           // The last argument is the clone destination.
           "  clone)\n"
           "    dst=\"\"\n"
           "    for a in \"$@\"; do dst=\"$a\"; done\n"
           "    mkdir -p \"$dst/.git\"\n"
           "    exit 0;;\n"
           "  fetch)\n"
           "    exit 0;;\n"
           // Records the checked-out commit where a real repo would keep its
           // HEAD, for the test to read.
           "  checkout)\n"
           "    commit=\"\"\n"
           "    for a in \"$@\"; do commit=\"$a\"; done\n"
           "    mkdir -p .git\n"
           "    echo \"$commit\" > .git/checked_out\n"
           "    exit 0;;\n"
           "  clean)\n"
           "    exit 0;;\n"
           "esac\n"
           "exit 1\n";
  }

  static auto ReadFile(const std::filesystem::path &path) -> std::string {
    std::ifstream in(path);
    if (!in) {
      return {};
    }
    return {std::istreambuf_iterator<char>(in),
            std::istreambuf_iterator<char>()};
  }

  static void ExpectLogContains(const std::string &log,
                                const std::string &fragment) {
    EXPECT_NE(log.find(fragment), std::string::npos) << "fragment: " << fragment;
  }

  static auto MakeOrder(const std::string &id, const std::string &candidate)
      -> proto::WorkOrder {
    proto::WorkOrder order;
    order.set_order_id(id);
    order.set_candidate_id(candidate);
    order.set_game("risk2");
    order.set_base_commit(kFakeCommit);
    auto *file = order.add_files();
    file->set_path("strategy.h");
    file->set_content("#pragma once\n");
    order.set_entry_header("strategy.h");
    order.set_broker_target("localhost:50051");
    order.set_opponent("builtin:random");
    order.set_num_games(2);
    (*order.mutable_params())["iterations"] = "100";
    return order;
  }

  static std::filesystem::path root_;
  static std::filesystem::path fake_docker_;
  static std::filesystem::path fake_git_;
  static std::unique_ptr<DockerBackend> backend_;
};

std::filesystem::path DockerBackendIntegrationTest::root_;
std::filesystem::path DockerBackendIntegrationTest::fake_docker_;
std::filesystem::path DockerBackendIntegrationTest::fake_git_;
std::unique_ptr<DockerBackend> DockerBackendIntegrationTest::backend_;

TEST_F(DockerBackendIntegrationTest, WarmupClonesOneLowerDirPerSlot) {
  // The clone is a host-side git step, not a docker one.
  const std::string git_log = ReadFile(root_ / "git.log");
  ExpectLogContains(
      git_log, "git clone --local " + (root_ / "repo_src").string() + " " +
                   (root_ / "work" / "slot0" / "repo").string());
  ExpectLogContains(
      git_log, "git clone --local " + (root_ / "repo_src").string() + " " +
                   (root_ / "work" / "slot1" / "repo").string());
  EXPECT_TRUE(std::filesystem::exists(root_ / "work" / "slot0" / "repo" / ".git"));
  EXPECT_TRUE(std::filesystem::exists(root_ / "work" / "slot1" / "repo" / ".git"));
  // Bind-mount sources that docker would have to create exist up front.
  EXPECT_TRUE(std::filesystem::is_directory(root_ / "work" / "disk_cache"));
  EXPECT_TRUE(std::filesystem::is_directory(root_ / "work" / "slot0" / "overlay"));
  EXPECT_TRUE(std::filesystem::is_directory(
      root_ / "work" / "slot0" / "bazel_output_base"));
}

TEST_F(DockerBackendIntegrationTest, OrderBuildsInContainerAndParsesResult) {
  const OrderOutcome outcome =
      backend_->RunOrder(0, MakeOrder("ok-1", "c-ok"));

  EXPECT_TRUE(outcome.build_ok) << outcome.error;
  EXPECT_TRUE(outcome.error.empty()) << outcome.error;
  EXPECT_EQ(outcome.games_played, 2);
  EXPECT_EQ(outcome.wins, 1);
  EXPECT_EQ(outcome.draws, 1);
  EXPECT_EQ(outcome.losses, 0);
  EXPECT_DOUBLE_EQ(outcome.elo, 1500.0);

  // The host checked the slot's clone out at the order's commit.
  const std::string git_log = ReadFile(root_ / "git.log");
  ExpectLogContains(git_log, "git fetch --all --tags --quiet");
  ExpectLogContains(git_log,
                    std::string("git checkout --force ") + kFakeCommit);
  EXPECT_EQ(ReadFile(root_ / "work" / "slot0" / "repo" / ".git" / "checked_out")
                .find(kFakeCommit),
            0);

  // The candidate and its generated BUILD are staged where docker is pointed.
  const auto staged = root_ / "work" / "slot0" / "patches" /
                      "game_mcts/tournament_server/candidates/c-ok";
  EXPECT_TRUE(std::filesystem::is_regular_file(staged / "strategy.h"));
  EXPECT_TRUE(std::filesystem::is_regular_file(staged / "BUILD"));

  const std::string log = ReadFile(root_ / "docker.log");
  // Zombie cleanup, then the build container over the slot's overlay.
  ExpectLogContains(log, "docker rm -f saw-0-ok-1-build");
  ExpectLogContains(log, "--name saw-0-ok-1-build");
  ExpectLogContains(log, "--cap-add SYS_ADMIN");
  ExpectLogContains(log, "--network host");
  ExpectLogContains(log, "--mount type=bind,source=" +
                             (root_ / "work" / "slot0" / "repo").string() +
                             ",target=/repo_lower,readonly");
  ExpectLogContains(log, "--mount type=bind,source=" +
                             (root_ / "work" / "slot0" / "overlay").string() +
                             ",target=/sandbox");
  ExpectLogContains(log, "--mount type=bind,source=" +
                             (root_ / "work" / "slot0" / "patches").string() +
                             ",target=/patches,readonly");
  ExpectLogContains(log, "--mount type=bind,source=" +
                             (root_ / "work" / "slot0" / "bazel_output_base")
                                 .string() +
                             ",target=/output_base");
  ExpectLogContains(log, "--mount type=bind,source=" +
                             (root_ / "work" / "disk_cache").string() +
                             ",target=/disk_cache");
  ExpectLogContains(log, "--entrypoint /bin/sh fake-image:1 -c");
  // The entrypoint: overlay assembly, candidate copy, bazel against the slot's
  // persistent output base.
  ExpectLogContains(log, "mount -t overlay overlay -o lowerdir=/repo_lower,"
                         "upperdir=/sandbox/upper,workdir=/sandbox/work "
                         "/workspace");
  ExpectLogContains(log, "cp -a /patches/. /workspace/");
  ExpectLogContains(log, "exec bazel --output_base=/output_base "
                         "--disk_cache=/disk_cache "
                         "build '//game_mcts/tournament_server/candidates/"
                         "c-ok:bot'");

  // The run container: same overlay and output base, a memory cap, and the
  // built bot exec'd with the broker connection.
  ExpectLogContains(log, "docker rm -f saw-0-ok-1-run");
  ExpectLogContains(log, "--name saw-0-ok-1-run");
  ExpectLogContains(log, "--memory 4096m");
  ExpectLogContains(log, "exec './bazel-bin/game_mcts/tournament_server/"
                         "candidates/c-ok/bot' '--name=c-ok' "
                         "'--server=localhost:50051' "
                         "'--opponent=builtin:random' '--games=2' "
                         "'--params=iterations=100'");
  // The run container does not carry the patch or disk-cache mounts.
  const auto run_invocation = log.find("--name saw-0-ok-1-run");
  ASSERT_NE(run_invocation, std::string::npos);
  const std::string run_line = log.substr(run_invocation, 4000);
  EXPECT_EQ(run_line.find("/patches"), std::string::npos);
  EXPECT_EQ(run_line.find("/disk_cache"), std::string::npos);
}

TEST_F(DockerBackendIntegrationTest, BuildFailureIsReportedNotErrored) {
  const OrderOutcome outcome =
      backend_->RunOrder(0, MakeOrder("failbuild-1", "failbuild-1"));

  // A build failure is the candidate's fault: a completed order with the
  // compacted diagnostics, nothing to play.
  EXPECT_FALSE(outcome.build_ok);
  EXPECT_TRUE(outcome.error.empty()) << outcome.error;
  EXPECT_EQ(outcome.games_played, 0);
  EXPECT_NE(outcome.build_log.find("expected ';' before '}' token"),
            std::string::npos);
  // The run phase never started.
  EXPECT_EQ(ReadFile(root_ / "docker.log").find("saw-0-failbuild-1-run"),
            std::string::npos);
}

TEST_F(DockerBackendIntegrationTest, RunTimeoutKillsTheContainer) {
  proto::WorkOrder order = MakeOrder("blockrun-1", "blockrun-1");
  order.set_run_timeout_s(2);

  const OrderOutcome outcome = backend_->RunOrder(0, order);

  EXPECT_TRUE(outcome.build_ok) << outcome.error;
  EXPECT_NE(outcome.error.find("games timed out after 2s"), std::string::npos)
      << outcome.error;
  // The server stopped the container by name after the client-side timeout.
  ExpectLogContains(ReadFile(root_ / "docker.log"),
                    "docker kill saw-0-blockrun-1-run");
}

TEST_F(DockerBackendIntegrationTest, EscapingPatchPathIsRejected) {
  proto::WorkOrder order = MakeOrder("esc-1", "esc-1");
  order.mutable_files(0)->set_path("../evil.h");
  order.set_entry_header("../evil.h");

  const OrderOutcome outcome = backend_->RunOrder(0, order);

  EXPECT_FALSE(outcome.build_ok);
  EXPECT_NE(outcome.error.find("escapes its directory"), std::string::npos)
      << outcome.error;
  // Rejected before anything reached docker.
  EXPECT_EQ(ReadFile(root_ / "docker.log").find("saw-0-esc-1"),
            std::string::npos);
}

TEST_F(DockerBackendIntegrationTest, WarmupValidatesTheRepo) {
  DockerBackendConfig config;
  config.docker = fake_docker_.string();
  config.git = fake_git_.string();
  config.docker_image = "fake-image:1";
  config.work_dir = root_ / "work_norepo";

  config.repo_dir = root_ / "does_not_exist";
  DockerBackend missing(config);
  std::string error;
  EXPECT_FALSE(missing.Warmup(1, &error));
  EXPECT_NE(error.find("not a directory"), std::string::npos) << error;

  const std::filesystem::path plain = root_ / "not_a_repo";
  std::filesystem::create_directories(plain);
  config.repo_dir = plain;
  DockerBackend not_git(config);
  error.clear();
  EXPECT_FALSE(not_git.Warmup(1, &error));
  EXPECT_NE(error.find("not a git repository"), std::string::npos) << error;
}

}  // namespace
}  // namespace tournament_arena
