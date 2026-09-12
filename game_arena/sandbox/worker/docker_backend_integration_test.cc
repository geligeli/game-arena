// End-to-end test of DockerBackend against fake `docker` and `git` scripts:
// shell scripts that log every invocation and emulate just enough of each
// tool (a clone that creates a .git dir, a checkout that records its commit,
// containers that behave per container name). The overlay mount never
// actually happens, but the exact mounts, argv and entrypoint scripts handed
// to docker, and the host-side git work, are asserted from the logs.

#include <gtest/gtest.h>
#include <unistd.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <system_error>

#include "game_arena/sandbox/worker/docker_backend.h"

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
    std::filesystem::permissions(fake_git_, std::filesystem::perms::owner_all,
                                 std::filesystem::perm_options::add);

    // The overlay is assembled on the host by default, so containers need no
    // CAP_SYS_ADMIN. Faked here: a test sandbox cannot mount(2), and what is
    // under test is the flags the backend passes, not the kernel.
    fake_mount_ = root_ / "fake_mount";
    std::ofstream(fake_mount_)
        << "#!/usr/bin/env bash\necho \"mount $*\" >> \""
        << (root_ / "mount.log").string() << "\"\nexit 0\n";
    std::filesystem::permissions(fake_mount_, std::filesystem::perms::owner_all,
                                 std::filesystem::perm_options::add);

    DockerBackendConfig config;
    config.docker = fake_docker_.string();
    config.git = fake_git_.string();
    config.mount = fake_mount_.string();
    config.umount = fake_mount_.string();
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
           "        echo "
           "\"solutions/failbuild-1/"
           "strategy.h:3:5: error: expected ';' before '}' token\"\n"
           "        echo \"Target "
           "//solutions/failbuild-1:bot failed to "
           "build\"\n"
           "        exit 1;;\n"
           // A bot that never finishes: runs until the server-side timeout
           // kills the container. bash execs a sole command, which would drop
           // the marker from the command line; the trailing `true` keeps
           // bash alive so Kill's pkill finds it.
           "      *blockrun-1-bot*)\n"
           "        bash -c 'sleep 15; true' \"fake-sleeper-$name\"\n"
           "        exit 137;;\n"
           // The referee is started detached and its verdict is read back with
           // `docker logs`, so stash it where the logs branch can find it.
           "      *-referee)\n"
           "        echo \"RESULT games=2 wins=1 draws=1 losses=0 elo=1500.0\" "
           "> \"" +
           (root_ / "referee_output").string() +
           "\"\n"
           "        exit 0;;\n"
           // The bot plays and exits; the tally comes from the referee.
           "      *-bot|*-opponent)\n"
           "        exit 0;;\n"
           // A clean build.
           "      *-build)\n"
           "        echo \"INFO: Build completed successfully, 42 total "
           "actions\"\n"
           "        exit 0;;\n"
           "    esac;;\n"
           "  rm)\n"
           "    exit 0;;\n"
           "  network)\n"
           "    exit 0;;\n"
           "  wait)\n"
           "    exit 0;;\n"
           "  logs)\n"
           "    cat \"" +
           (root_ / "referee_output").string() +
           "\" 2>/dev/null\n"
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
    EXPECT_NE(log.find(fragment), std::string::npos)
        << "fragment: " << fragment;
  }

  static auto MakeOrder(const std::string &id,
                        const std::string &candidate) -> proto::WorkOrder {
    proto::WorkOrder order;
    order.set_order_id(id);
    order.set_game("nim");
    order.set_base_commit(kFakeCommit);
    order.set_referee_target("//game_arena/testgame:match_referee");
    order.set_opponent_spec("builtin:random");
    order.set_num_games(2);

    proto::Side *side = order.mutable_candidate();
    side->set_candidate_id(candidate);
    side->set_patch("diff --git a/solutions/" + candidate +
                    "/strategy.h b/solutions/" + candidate +
                    "/strategy.h\n"
                    "new file mode 100644\n"
                    "--- /dev/null\n"
                    "+++ b/solutions/" +
                    candidate +
                    "/strategy.h\n"
                    "@@ -0,0 +1,1 @@\n"
                    "+#pragma once\n");
    side->add_build_targets("//solutions/" + candidate + ":bot");
    side->set_bot_target("//solutions/" + candidate + ":bot");
    (*side->mutable_params())["iterations"] = "100";
    return order;
  }

  static std::filesystem::path root_;
  static std::filesystem::path fake_docker_;
  static std::filesystem::path fake_git_;
  static std::filesystem::path fake_mount_;
  static std::unique_ptr<DockerBackend> backend_;
};

std::filesystem::path DockerBackendIntegrationTest::root_;
std::filesystem::path DockerBackendIntegrationTest::fake_docker_;
std::filesystem::path DockerBackendIntegrationTest::fake_git_;
std::filesystem::path DockerBackendIntegrationTest::fake_mount_;
std::unique_ptr<DockerBackend> DockerBackendIntegrationTest::backend_;

TEST_F(DockerBackendIntegrationTest, WarmupClonesOneLowerDirPerSlot) {
  // The clone is a host-side git step, not a docker one.
  const std::string git_log = ReadFile(root_ / "git.log");
  ExpectLogContains(git_log, "git clone --local " +
                                 (root_ / "repo_src").string() + " " +
                                 (root_ / "work" / "slot0" / "repo").string());
  ExpectLogContains(git_log, "git clone --local " +
                                 (root_ / "repo_src").string() + " " +
                                 (root_ / "work" / "slot1" / "repo").string());
  EXPECT_TRUE(
      std::filesystem::exists(root_ / "work" / "slot0" / "repo" / ".git"));
  EXPECT_TRUE(
      std::filesystem::exists(root_ / "work" / "slot1" / "repo" / ".git"));
  // Bind-mount sources that docker would have to create exist up front.
  EXPECT_TRUE(std::filesystem::is_directory(root_ / "work" / "disk_cache"));
  EXPECT_TRUE(
      std::filesystem::is_directory(root_ / "work" / "slot0" / "overlay"));
  EXPECT_TRUE(std::filesystem::is_directory(root_ / "work" / "slot0" /
                                            "bazel_output_base"));
}

// The problem's registry_options have to survive all the way to the referee's
// argv. They used to not: the config carried an mcts_iterations field that the
// coordinator defaulted and no worker ever passed on, so a problem asking for a
// stronger builtin was silently ignored because the referee's own flag default
// happened to match.
TEST_F(DockerBackendIntegrationTest, RegistryOptionsReachTheReferee) {
  proto::WorkOrder order = MakeOrder("opts-1", "c-ok");
  (*order.mutable_registry_options())["mcts_iterations"] = "800";
  (*order.mutable_registry_options())["depth"] = "7";
  ASSERT_TRUE(backend_->RunOrder(0, order).build_ok);

  ExpectLogContains(ReadFile(root_ / "docker.log"),
                    "'--registry_options=depth=7,mcts_iterations=800'");
}

// And an order that sets none must produce exactly the argv it always did.
TEST_F(DockerBackendIntegrationTest, NoRegistryOptionsMeansNoFlag) {
  // The fake docker log is shared by the whole suite, so look only at what
  // this order appended to it.
  const std::size_t before = ReadFile(root_ / "docker.log").size();
  ASSERT_TRUE(backend_->RunOrder(0, MakeOrder("noopts-1", "c-ok")).build_ok);

  const std::string mine = ReadFile(root_ / "docker.log").substr(before);
  ASSERT_NE(mine.find("saw-0-noopts-1-referee"), std::string::npos)
      << "the order did not run; nothing was asserted";
  EXPECT_EQ(mine.find("--registry_options"), std::string::npos) << mine;
}

TEST_F(DockerBackendIntegrationTest, OrderBuildsInContainerAndParsesResult) {
  const OrderOutcome outcome = backend_->RunOrder(0, MakeOrder("ok-1", "c-ok"));

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

  // The submission is staged as the one thing the container applies: its patch.
  const auto staged = root_ / "work" / "slot0" / "patches" / "c-ok.diff";
  ASSERT_TRUE(std::filesystem::is_regular_file(staged));
  EXPECT_NE(ReadFile(staged).find("+#pragma once"), std::string::npos);

  const std::string log = ReadFile(root_ / "docker.log");
  // Zombie cleanup, then the build container over the slot's overlay.
  ExpectLogContains(log, "docker rm -f saw-0-ok-1-build");
  ExpectLogContains(log, "--name saw-0-ok-1-build");
  // No capabilities: the overlay was mounted on the host, so there is nothing
  // for the container to be privileged for.
  ExpectLogContains(log, "--cap-drop ALL");
  ExpectLogContains(log, "--security-opt no-new-privileges");
  EXPECT_EQ(log.find("SYS_ADMIN"), std::string::npos) << log;
  // A build that can fetch can also exfiltrate, and a submitted genrule is
  // arbitrary code.
  ExpectLogContains(log, "--network none");
  EXPECT_EQ(log.find("--network host"), std::string::npos) << log;
  // The container gets the merged tree, not the pieces: with the overlay
  // mounted on the host there is nothing left for it to assemble.
  ExpectLogContains(log, "--mount type=bind,source=" +
                             (root_ / "work" / "slot0" / "merged").string() +
                             ",target=/workspace");
  ExpectLogContains(ReadFile(root_ / "mount.log"), "-t overlay");
  ExpectLogContains(log, "--mount type=bind,source=" +
                             (root_ / "work" / "slot0" / "overlay").string() +
                             ",target=/sandbox");
  ExpectLogContains(log, "--mount type=bind,source=" +
                             (root_ / "work" / "slot0" / "patches").string() +
                             ",target=/patches,readonly");
  ExpectLogContains(
      log, "--mount type=bind,source=" +
               (root_ / "work" / "slot0" / "bazel_output_base").string() +
               ",target=/output_base");
  ExpectLogContains(log, "--mount type=bind,source=" +
                             (root_ / "work" / "disk_cache").string() +
                             ",target=/disk_cache");
  ExpectLogContains(log, "--entrypoint /bin/sh fake-image:1 -c");
  // The entrypoint applies the patch and builds; it mounts nothing.
  EXPECT_EQ(log.find("mount -t overlay"), std::string::npos)
      << "the container should not be mounting anything:\n"
      << log;
  ExpectLogContains(log,
                    "exec bazel --output_base=/output_base "
                    "--disk_cache=/disk_cache "
                    "build '//solutions/"
                    "c-ok:bot'");

  // The match runs on its own egress-free network, torn down afterwards.
  ExpectLogContains(log, "docker network create --internal saw-0-ok-1-net");
  ExpectLogContains(log, "docker network rm saw-0-ok-1-net");

  // The referee is started detached on that network, and judges the match.
  ExpectLogContains(log, "--name saw-0-ok-1-referee");
  ExpectLogContains(log, "--network saw-0-ok-1-net");
  ExpectLogContains(log,
                    "exec './bazel-bin/game_arena/testgame/"
                    "match_referee' '--port=50051' '--game=nim' "
                    "'--games=2' '--player_a=c-ok' "
                    "'--player_b=builtin:random'");

  // The bot dials the referee by container name, not a host address: there is
  // no broker outside the sandbox to reach.
  ExpectLogContains(log, "--name saw-0-ok-1-bot");
  ExpectLogContains(log, "--memory 4096m");
  ExpectLogContains(log,
                    "exec './bazel-bin/solutions/c-ok/bot' '--name=c-ok' "
                    "'--server=saw-0-ok-1-referee:50051' "
                    "'--opponent=builtin:random' '--games=2' "
                    "'--params=iterations=100'");

  // The verdict is read from the referee, not the bot.
  ExpectLogContains(log, "docker wait saw-0-ok-1-referee");
  ExpectLogContains(log, "docker logs saw-0-ok-1-referee");

  // The playing containers do not carry the patch or disk-cache mounts.
  const auto run_invocation = log.find("--name saw-0-ok-1-bot");
  ASSERT_NE(run_invocation, std::string::npos);
  const std::string run_line = log.substr(run_invocation, 4000);
  EXPECT_EQ(run_line.find("/patches"), std::string::npos);
  EXPECT_EQ(run_line.find("/disk_cache"), std::string::npos);
}

// The isolation this backend rests on, asserted as flags rather than trusted
// as a comment. A submitted genrule is arbitrary code; the claim is that it
// runs with nothing to reach and nothing to keep.
TEST_F(DockerBackendIntegrationTest, EveryContainerIsHardened) {
  ASSERT_TRUE(backend_->RunOrder(0, MakeOrder("hard-1", "c-hard")).build_ok);
  const std::string log = ReadFile(root_ / "docker.log");

  int hardened = 0;
  std::size_t at = 0;
  while ((at = log.find("docker run", at)) != std::string::npos) {
    const std::size_t end = log.find('\n', at);
    const std::string line = log.substr(at, end - at);
    at = end == std::string::npos ? log.size() : end;
    EXPECT_NE(line.find("--cap-drop ALL"), std::string::npos) << line;
    EXPECT_NE(line.find("--security-opt no-new-privileges"), std::string::npos)
        << line;
    EXPECT_NE(line.find("--read-only"), std::string::npos) << line;
    EXPECT_NE(line.find("--pids-limit"), std::string::npos) << line;
    // Never the host's network: the build and a graded run get none, and a
    // match gets its own internal bridge.
    EXPECT_EQ(line.find("--network host"), std::string::npos) << line;
    ++hardened;
  }
  EXPECT_GE(hardened, 2) << "expected at least a build and a run container";
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
                    "docker kill saw-0-blockrun-1-bot");
}

// A side with no patch cannot be staged, and nothing should reach docker.
// The path-escape check itself now lives at submit time, in the diff parser --
// the worker's job is to notice it has nothing to apply.
TEST_F(DockerBackendIntegrationTest, SideWithoutAPatchIsRejected) {
  proto::WorkOrder order = MakeOrder("esc-1", "esc-1");
  order.mutable_candidate()->clear_patch();

  const OrderOutcome outcome = backend_->RunOrder(0, order);

  EXPECT_FALSE(outcome.build_ok);
  EXPECT_NE(outcome.error.find("carries no patch"), std::string::npos)
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
