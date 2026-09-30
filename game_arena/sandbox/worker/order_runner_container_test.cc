// End-to-end test of OrderRunner on the container engine, against a fake
// `docker`: a shell script that logs every invocation and emulates just
// enough of it (containers that behave per container name). Nothing is really
// loaded into a volume, but the exact volumes, mounts, argv and entrypoint
// scripts handed to docker are asserted from the log.

#include <grpcpp/grpcpp.h>
#include <gtest/gtest.h>
#include <unistd.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <system_error>

#include "game_arena/proto/arena.grpc.pb.h"
#include "game_arena/proto/tournament_broker.pb.h"
#include "game_arena/sandbox/exec/container_engine.h"
#include "game_arena/sandbox/worker/artifact_cache.h"
#include "game_arena/sandbox/worker/order_runner.h"

namespace tournament_arena {
namespace {

namespace proto = tournament_arena::proto;

class OrderRunnerContainerTest : public ::testing::Test {
 protected:
  static void SetUpTestSuite() {
    root_ = std::filesystem::temp_directory_path() /
            ("order_runner_container_itest_" + std::to_string(::getpid()));
    std::filesystem::create_directories(root_);

    fake_docker_ = root_ / "fake_docker";
    std::ofstream(fake_docker_) << FakeDockerScript();
    std::filesystem::permissions(fake_docker_,
                                 std::filesystem::perms::owner_all,
                                 std::filesystem::perm_options::add);

    // What every referee leaves in its scratch: a win and a draw for c-ok.
    tournament_broker::proto::MatchReport report;
    for (const auto result : {tournament_broker::proto::GameRecord::WIN,
                              tournament_broker::proto::GameRecord::DRAW}) {
      tournament_broker::proto::GameRecord *game = report.add_games();
      game->add_player_names("c-ok");
      game->add_player_names("builtin:random");
      game->set_result(result);
    }
    std::ofstream out(root_ / "match.pb", std::ios::binary);
    report.SerializeToOstream(&out);

    sandbox_exec::ContainerEngineConfig engine_config;
    engine_config.docker = fake_docker_.string();
    engine_ = std::make_unique<sandbox_exec::ContainerEngine>(engine_config);
  }

  // A runner per test, so no test inherits another's state.
  void SetUp() override {
    OrderJobConfig config;
    config.work_dir = root_ / "work";
    config.disk_cache = root_ / "work" / "disk_cache";
    runner_ = std::make_unique<OrderRunner>(/*process_engine=*/nullptr,
                                            engine_.get(), std::move(config));

    std::string error;
    ASSERT_TRUE(runner_->Warmup(2, &error)) << error;
  }

  static void TearDownTestSuite() {
    runner_.reset();
    engine_.reset();
    std::error_code ec;
    std::filesystem::remove_all(root_, ec);
  }

  static std::string FakeDockerScript() {
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
           // The referee is started detached; its report is copied out of its
           // scratch afterwards (the cp branch).
           "      *-referee)\n"
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
           "  cp)\n"
           "    case \"$1\" in\n"
           "      *-referee:/sandbox/match.pb) cp \"" +
           (root_ / "match.pb").string() +
           "\" \"$2\";;\n"
           // A build order's binaries, as its stash step left them.
           "      *-stash:/sandbox/*) echo \"stashed $1\" > \"$2\";;\n"
           "    esac\n"
           "    exit 0;;\n"
           "  rm|create|start|volume|network|wait|logs)\n"
           "    exit 0;;\n"
           "  kill)\n"
           "    pkill -f \"fake-sleeper-$1\"\n"
           "    exit 0;;\n"
           "esac\n"
           "exit 1\n";
  }

  static std::string ReadFile(const std::filesystem::path &path) {
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

  // The whole `docker run` argv for |container|, from "docker run" up to the
  // `-c` that introduces the entrypoint script.
  //
  // Every other assertion in this file is a substring match on one flag, which
  // cannot see a flag inserted in the middle of the argv or a reordered mount
  // list. This can. The script after `-c` is deliberately excluded: it spans
  // lines and is already asserted byte for byte in
  // //game_arena/sandbox/exec:entrypoint_test.
  static std::string RunArgvFor(const std::string &log,
                                const std::string &container) {
    const std::string name_flag = "--name " + container + " ";
    const std::size_t at = log.find(name_flag);
    if (at == std::string::npos) {
      return "<no `docker run` for " + container + ">";
    }
    const std::size_t start = log.rfind("docker run", at);
    const std::size_t end = log.find(" -c ", at);
    if (start == std::string::npos || end == std::string::npos) {
      return "<malformed `docker run` for " + container + ">";
    }
    return log.substr(start, end - start);
  }

  static proto::WorkOrder MakeOrder(const std::string &id,
                                    const std::string &candidate) {
    proto::WorkOrder order;
    order.set_order_id(id);
    order.set_game("nim");
    order.set_referee_target("//game_arena/testgame:match_referee");
    order.add_opponent_spec("builtin:random");
    order.set_num_games(2);

    // The sandbox travels with the order: a worker has no image and no tree
    // of its own.
    proto::SandboxOrder *sandbox = order.mutable_sandbox();
    sandbox->set_image("fake-image:1");
    sandbox->set_memory_limit_mb(4096);
    sandbox->set_pids_limit(512);

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
  static std::unique_ptr<sandbox_exec::ContainerEngine> engine_;
  static std::unique_ptr<OrderRunner> runner_;
};

std::filesystem::path OrderRunnerContainerTest::root_;
std::filesystem::path OrderRunnerContainerTest::fake_docker_;
std::unique_ptr<sandbox_exec::ContainerEngine>
    OrderRunnerContainerTest::engine_;
std::unique_ptr<OrderRunner> OrderRunnerContainerTest::runner_;

TEST_F(OrderRunnerContainerTest, TheTreeIsTheImagesNotTheWorkers) {
  // A worker has no tree of its own: the job's fresh volume is mounted at the
  // workspace of a container made from the order's image, and docker fills a
  // fresh volume from what the image has there. Nothing is copied into it
  // from this side -- which is what stops two hosts in one fleet from
  // building a problem out of two different trees.
  const std::size_t before = ReadFile(root_ / "docker.log").size();
  ASSERT_TRUE(
      runner_->RunOrder(0, MakeOrder("tree-1", "c-ok"), {}).result.build_ok());

  const std::string mine = ReadFile(root_ / "docker.log").substr(before);
  ExpectLogContains(mine, "docker create --name saw-0-tree-1-load ");
  ExpectLogContains(mine, "source=saw-0-tree-1-ws,target=/workspace");
  EXPECT_EQ(mine.find(":/workspace"), std::string::npos) << mine;
  EXPECT_FALSE(std::filesystem::exists(root_ / "work" / "slot0" / "repo"));
  // Bind-mount sources that docker would otherwise conjure up exist up front.
  EXPECT_TRUE(std::filesystem::is_directory(root_ / "work" / "disk_cache"));
}

// The coordinator's archive, in this process.
class FakeArchive final : public proto::SandboxFleet::Service {
 public:
  grpc::Status PutArtifact(grpc::ServerContext *,
                           grpc::ServerReader<proto::ArtifactChunk> *reader,
                           proto::PutArtifactResponse *response) override {
    proto::ArtifactChunk chunk;
    std::string bytes;
    while (reader->Read(&chunk)) {
      bytes += chunk.data();
    }
    std::lock_guard lock(mutex_);
    blobs_[chunk.digest()] = bytes;
    response->set_digest(chunk.digest());
    return grpc::Status::OK;
  }
  grpc::Status GetArtifact(
      grpc::ServerContext *, const proto::GetArtifactRequest *request,
      grpc::ServerWriter<proto::ArtifactChunk> *writer) override {
    std::lock_guard lock(mutex_);
    const auto blob = blobs_.find(request->digest());
    if (blob == blobs_.end()) {
      return {grpc::StatusCode::NOT_FOUND, request->digest()};
    }
    proto::ArtifactChunk chunk;
    chunk.set_digest(blob->first);
    chunk.set_data(blob->second);
    writer->Write(chunk);
    return grpc::Status::OK;
  }
  std::size_t size() {
    std::lock_guard lock(mutex_);
    return blobs_.size();
  }

 private:
  std::mutex mutex_;
  std::map<std::string, std::string> blobs_;
};

TEST_F(OrderRunnerContainerTest, ABuildIsArchivedAndAMatchPlaysItUnbuilt) {
  FakeArchive archive;
  grpc::ServerBuilder builder;
  int port = 0;
  builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(),
                           &port);
  builder.RegisterService(&archive);
  const std::unique_ptr<grpc::Server> server = builder.BuildAndStart();
  const auto stub = proto::SandboxFleet::NewStub(grpc::CreateChannel(
      "127.0.0.1:" + std::to_string(port), grpc::InsecureChannelCredentials()));
  ArtifactCache cache(root_ / "artifacts", stub.get());
  OrderJobConfig config;
  config.work_dir = root_ / "work";
  OrderRunner runner(/*process_engine=*/nullptr, engine_.get(), config, "",
                     &cache);

  proto::WorkOrder build = MakeOrder("arc-1", "c-ok");
  build.set_build_only(true);
  const OrderOutcome built = runner.RunOrder(0, build, {});
  ASSERT_TRUE(built.result.build_ok()) << built.result.DebugString();
  ASSERT_EQ(built.result.error(), "");
  const auto &artifacts = built.result.artifacts();
  ASSERT_TRUE(artifacts.contains("//solutions/c-ok:bot"));
  ASSERT_TRUE(artifacts.contains("//game_arena/testgame:match_referee"));
  EXPECT_EQ(archive.size(), 2u);
  EXPECT_EQ(built.result.games_played(), 0);

  proto::WorkOrder match = MakeOrder("arc-2", "c-ok");
  match.mutable_candidate()->set_artifact(artifacts.at("//solutions/c-ok:bot"));
  match.set_referee_artifact(
      artifacts.at("//game_arena/testgame:match_referee"));
  const std::size_t before = ReadFile(root_ / "docker.log").size();
  const OrderOutcome played = runner.RunOrder(1, match, {});
  EXPECT_TRUE(played.result.build_ok()) << played.result.DebugString();
  EXPECT_EQ(played.result.games_played(), 2);

  const std::string mine = ReadFile(root_ / "docker.log").substr(before);
  EXPECT_EQ(mine.find("saw-1-arc-2-build"), std::string::npos) << mine;
  ExpectLogContains(mine, "docker cp " + (root_ / "artifacts").string() + "/" +
                              artifacts.at("//solutions/c-ok:bot") +
                              " saw-1-arc-2-load:/load_inputs/bot-c-ok");
  ExpectLogContains(mine, "source=saw-1-arc-2-inputs,target=/inputs,readonly");
  ExpectLogContains(mine, "exec '/inputs/bot-c-ok' '--name=c-ok'");
  server->Shutdown();
}

// The problem's registry_options have to survive all the way to the referee's
// argv. They used to not: the config carried an mcts_iterations field that the
// coordinator defaulted and no worker ever passed on, so a problem asking for a
// stronger builtin was silently ignored because the referee's own flag default
// happened to match.
TEST_F(OrderRunnerContainerTest, RegistryOptionsReachTheReferee) {
  proto::WorkOrder order = MakeOrder("opts-1", "c-ok");
  (*order.mutable_registry_options())["mcts_iterations"] = "800";
  (*order.mutable_registry_options())["depth"] = "7";
  ASSERT_TRUE(runner_->RunOrder(0, order, {}).result.build_ok());

  ExpectLogContains(ReadFile(root_ / "docker.log"),
                    "'--registry_options=depth=7,mcts_iterations=800'");
}

// And an order that sets none must produce exactly the argv it always did.
TEST_F(OrderRunnerContainerTest, NoRegistryOptionsMeansNoFlag) {
  // The fake docker log is shared by the whole suite, so look only at what
  // this order appended to it.
  const std::size_t before = ReadFile(root_ / "docker.log").size();
  ASSERT_TRUE(runner_->RunOrder(0, MakeOrder("noopts-1", "c-ok"), {})
                  .result.build_ok());

  const std::string mine = ReadFile(root_ / "docker.log").substr(before);
  ASSERT_NE(mine.find("saw-0-noopts-1-referee"), std::string::npos)
      << "the order did not run; nothing was asserted";
  EXPECT_EQ(mine.find("--registry_options"), std::string::npos) << mine;
}

TEST_F(OrderRunnerContainerTest, OrderBuildsInContainerAndParsesResult) {
  const OrderOutcome outcome =
      runner_->RunOrder(0, MakeOrder("ok-1", "c-ok"), {});

  EXPECT_TRUE(outcome.result.build_ok()) << outcome.result.error();
  EXPECT_TRUE(outcome.result.error().empty()) << outcome.result.error();
  EXPECT_EQ(outcome.result.games_played(), 2);
  EXPECT_EQ(outcome.result.wins(), 1);
  EXPECT_EQ(outcome.result.draws(), 1);
  EXPECT_EQ(outcome.result.losses(), 0);

  // The submission is staged as the one thing the container applies: its patch.
  const auto staged = root_ / "work" / "slot0" / "patches" / "c-ok.diff";
  ASSERT_TRUE(std::filesystem::is_regular_file(staged));
  EXPECT_NE(ReadFile(staged).find("+#pragma once"), std::string::npos);

  const std::string log = ReadFile(root_ / "docker.log");
  // The staged patch is loaded into the job's own volume through the daemon;
  // nothing on this side is mounted anywhere.
  ExpectLogContains(log, "docker volume create saw-0-ok-1-ws");
  ExpectLogContains(log, "docker volume create saw-0-ok-1-patches");
  ExpectLogContains(log, "docker volume create saw-0-ok-1-scratch");
  ExpectLogContains(log, "docker cp " +
                             (root_ / "work" / "slot0" / "patches").string() +
                             "/. saw-0-ok-1-load:/patches");
  ExpectLogContains(log, "docker start -a saw-0-ok-1-load");
  EXPECT_EQ(log.find("type=bind"), std::string::npos) << log;
  // Zombie cleanup, then the build container on the loaded tree.
  ExpectLogContains(log, "docker rm -f saw-0-ok-1-build");
  ExpectLogContains(log, "--name saw-0-ok-1-build");
  // No capabilities: nothing was mounted, so there is nothing for the
  // container to be privileged for.
  ExpectLogContains(log, "--cap-drop ALL");
  ExpectLogContains(log, "--security-opt no-new-privileges");
  EXPECT_EQ(log.find("SYS_ADMIN"), std::string::npos) << log;
  // A build that can fetch can also exfiltrate, and a submitted genrule is
  // arbitrary code.
  ExpectLogContains(log, "--network none");
  EXPECT_EQ(log.find("--network host"), std::string::npos) << log;
  ExpectLogContains(log,
                    "--mount type=volume,source=saw-0-ok-1-ws,"
                    "target=/workspace");
  ExpectLogContains(log,
                    "--mount type=volume,source=saw-0-ok-1-scratch,"
                    "target=/sandbox");
  ExpectLogContains(log,
                    "--mount type=volume,source=saw-0-ok-1-patches,"
                    "target=/patches,readonly");
  ExpectLogContains(log,
                    "--mount type=volume,source=arena-slot0-output_base,"
                    "target=/output_base");
  ExpectLogContains(log,
                    "--mount type=volume,source=arena-disk_cache,"
                    "target=/disk_cache");
  ExpectLogContains(log, "--entrypoint /bin/sh fake-image:1 -c");
  // The entrypoint applies the patch and builds; it mounts nothing.
  EXPECT_EQ(log.find("mount -t overlay"), std::string::npos)
      << "the container should not be mounting anything:\n"
      << log;
  // The job's volumes go with the job; the caches stay.
  ExpectLogContains(log, "docker volume rm -f saw-0-ok-1-ws");
  EXPECT_EQ(log.find("volume rm -f arena-"), std::string::npos) << log;
  // --output_base before the command, --disk_cache after it: one is a bazel
  // startup option and the other is not, and the container path used to get
  // that wrong -- which is why it never built anything. An output_user_root
  // rather than an --install_base: bazel keys the install directory under it
  // by its own binary, so a volume that outlives the image survives the
  // image moving to another bazel.
  ExpectLogContains(log,
                    "exec bazel --output_base=/output_base "
                    "--output_user_root=/output_base/_user_root build "
                    "--disk_cache=/disk_cache '//solutions/"
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

  // The verdict is the referee's report, from a scratch volume only it
  // mounts: made for it, handed to the sandbox's user, and gone with the job.
  ExpectLogContains(log, "docker wait saw-0-ok-1-referee");
  ExpectLogContains(log, "docker cp saw-0-ok-1-referee:/sandbox/match.pb ");
  ExpectLogContains(log, "'--report=/sandbox/match.pb'");
  ExpectLogContains(log, "docker volume create saw-0-ok-1-referee-scratch");
  ExpectLogContains(log,
                    "--mount type=volume,source=saw-0-ok-1-referee-scratch,"
                    "target=/private_scratch/referee");
  ExpectLogContains(log, "docker volume rm -f saw-0-ok-1-referee-scratch");

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
TEST_F(OrderRunnerContainerTest, EveryContainerIsHardened) {
  ASSERT_TRUE(runner_->RunOrder(0, MakeOrder("hard-1", "c-hard"), {})
                  .result.build_ok());
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
    // The pid cap is the solution's, not the build's.
    if (line.find("-build ") == std::string::npos) {
      EXPECT_NE(line.find("--pids-limit"), std::string::npos) << line;
    }
    // Never the host's network: the build and a graded run get none, and a
    // match gets its own internal bridge.
    EXPECT_EQ(line.find("--network host"), std::string::npos) << line;
    // And nothing of this host's filesystem, unless a host opts in.
    EXPECT_EQ(line.find("type=bind"), std::string::npos) << line;
    ++hardened;
  }
  EXPECT_GE(hardened, 2) << "expected at least a build and a run container";
}

TEST_F(OrderRunnerContainerTest, BuildFailureIsReportedNotErrored) {
  const OrderOutcome outcome =
      runner_->RunOrder(0, MakeOrder("failbuild-1", "failbuild-1"), {});

  // A build failure is the candidate's fault: a completed order with the
  // compacted diagnostics, nothing to play.
  EXPECT_FALSE(outcome.result.build_ok());
  EXPECT_TRUE(outcome.result.error().empty()) << outcome.result.error();
  EXPECT_EQ(outcome.result.games_played(), 0);
  EXPECT_NE(outcome.result.build_log().find("expected ';' before '}' token"),
            std::string::npos);
  // The run phase never started.
  EXPECT_EQ(ReadFile(root_ / "docker.log").find("saw-0-failbuild-1-run"),
            std::string::npos);
}

TEST_F(OrderRunnerContainerTest, RunTimeoutKillsTheContainer) {
  proto::WorkOrder order = MakeOrder("blockrun-1", "blockrun-1");
  order.set_run_timeout_s(2);

  const OrderOutcome outcome = runner_->RunOrder(0, order, {});

  EXPECT_TRUE(outcome.result.build_ok()) << outcome.result.error();
  EXPECT_NE(outcome.result.error().find("games timed out after 2s"),
            std::string::npos)
      << outcome.result.error();
  // The server stopped the container by name after the client-side timeout.
  ExpectLogContains(ReadFile(root_ / "docker.log"),
                    "docker kill saw-0-blockrun-1-bot");
}

// A side with no patch cannot be staged, and nothing should reach docker.
// The path-escape check itself now lives at submit time, in the diff parser --
// the worker's job is to notice it has nothing to apply.
TEST_F(OrderRunnerContainerTest, SideWithoutAPatchIsRejected) {
  proto::WorkOrder order = MakeOrder("esc-1", "esc-1");
  order.mutable_candidate()->clear_patch();

  const OrderOutcome outcome = runner_->RunOrder(0, order, {});

  EXPECT_FALSE(outcome.result.build_ok());
  EXPECT_NE(outcome.result.error().find("carries no patch"), std::string::npos)
      << outcome.result.error();
  // Rejected before anything reached docker.
  EXPECT_EQ(ReadFile(root_ / "docker.log").find("saw-0-esc-1"),
            std::string::npos);
}

TEST_F(OrderRunnerContainerTest, WarmupMakesTheDirectoriesThisHostOwns) {
  OrderJobConfig config;
  config.work_dir = root_ / "work_warm";
  config.bind_output_base_dir = root_ / "bind_ob";
  config.bind_disk_cache_dir = root_ / "bind_dc";

  OrderRunner fresh(/*process_engine=*/nullptr, engine_.get(), config);
  std::string error;
  ASSERT_TRUE(fresh.Warmup(2, &error)) << error;

  // The process engine's per-slot state, and -- only because this host opted
  // into bind mounts -- their sources: docker conjures a missing one up as
  // an empty directory owned by root, which is a confusing way to learn the
  // path was wrong.
  for (const char *slot : {"slot0", "slot1"}) {
    EXPECT_TRUE(std::filesystem::is_directory(config.work_dir / slot /
                                              "bazel_output_base"));
    EXPECT_TRUE(
        std::filesystem::is_directory(config.work_dir / slot / "scratch"));
    EXPECT_TRUE(std::filesystem::is_directory(root_ / "bind_ob" / slot));
  }
  EXPECT_TRUE(std::filesystem::is_directory(root_ / "bind_dc"));
}

TEST_F(OrderRunnerContainerTest, AHostMayBindItsCachesForSpeed) {
  OrderJobConfig config;
  config.work_dir = root_ / "work_bind";
  config.bind_output_base_dir = root_ / "fast" / "ob";
  config.bind_disk_cache_dir = root_ / "fast" / "dc";
  OrderRunner bound(/*process_engine=*/nullptr, engine_.get(), config);
  std::string error;
  ASSERT_TRUE(bound.Warmup(1, &error)) << error;
  ASSERT_TRUE(
      bound.RunOrder(0, MakeOrder("bind-1", "c-ok"), {}).result.build_ok());

  const std::string log = ReadFile(root_ / "docker.log");
  const std::string build = RunArgvFor(log, "saw-0-bind-1-build");
  // The caches as the daemon's host paths; the tree and scratch still the
  // job's volumes, because those are what make the mounts optional.
  ExpectLogContains(build, "--mount type=bind,source=" +
                               (root_ / "fast" / "ob" / "slot0").string() +
                               ",target=/output_base");
  ExpectLogContains(
      build, "--mount type=bind,source=" + (root_ / "fast" / "dc").string() +
                 ",target=/disk_cache");
  ExpectLogContains(build,
                    "--mount type=volume,source=saw-0-bind-1-ws,"
                    "target=/workspace");
  // The loader chowns nothing it does not own: no bind mount is handed to it.
  const auto loader = log.find("docker create --name saw-0-bind-1-load");
  ASSERT_NE(loader, std::string::npos);
  const std::string loader_line =
      log.substr(loader, log.find('\n', loader) - loader);
  EXPECT_EQ(loader_line.find((root_ / "fast").string()), std::string::npos)
      << loader_line;
}

// The whole argv, not one flag of it.
//
// Every other docker assertion in this file matches a substring, so a flag
// inserted mid-argv, a dropped one, or a reordered mount list passes them all.
// These two pin the complete command line for the two container shapes an
// order starts, which is what makes a refactor of the backend reviewable: the
// emitted argv either is byte-identical or the diff says exactly how it moved.
TEST_F(OrderRunnerContainerTest, WholeDockerRunArgvIsPinned) {
  ASSERT_TRUE(
      runner_->RunOrder(0, MakeOrder("argv-1", "c-ok"), {}).result.build_ok());
  const std::string log = ReadFile(root_ / "docker.log");

  // The build: throwaway (--rm), no network at all, no memory or pid cap
  // (those are the solution's; bazel is the problem's own toolchain), and
  // the only container that mounts the patches and the shared disk cache.
  // Not one bind mount.
  EXPECT_EQ(RunArgvFor(log, "saw-0-argv-1-build"),
            "docker run --rm --name saw-0-argv-1-build "
            "--cap-drop ALL --security-opt no-new-privileges --read-only "
            "--tmpfs /tmp:exec "
            "--network none "
            "--mount type=volume,source=saw-0-argv-1-ws,target=/workspace "
            "--mount type=volume,source=saw-0-argv-1-scratch,target=/sandbox "
            "--mount type=volume,source=saw-0-argv-1-patches,"
            "target=/patches,readonly "
            "--mount type=volume,source=arena-slot0-output_base,"
            "target=/output_base "
            "--mount type=volume,source=arena-disk_cache,target=/disk_cache "
            "--entrypoint /bin/sh fake-image:1");

  // The bot: same hardening, joined to the order's private bridge instead of
  // no network, kept after it exits so its output can still be read, and with
  // nothing of the build's staging mounted, and the output base read-only.
  EXPECT_EQ(RunArgvFor(log, "saw-0-argv-1-bot"),
            "docker run --name saw-0-argv-1-bot "
            "--cap-drop ALL --security-opt no-new-privileges --read-only "
            "--tmpfs /tmp:exec --memory 4096m --pids-limit 512 "
            "--network saw-0-argv-1-net "
            "--mount type=volume,source=saw-0-argv-1-ws,target=/workspace "
            "--mount type=volume,source=saw-0-argv-1-scratch,target=/sandbox "
            "--mount type=volume,source=arena-slot0-output_base,"
            "target=/output_base,readonly "
            "--entrypoint /bin/sh fake-image:1");

  // The referee: the bot's, except that its scratch is its own.
  EXPECT_EQ(RunArgvFor(log, "saw-0-argv-1-referee"),
            "docker run --name saw-0-argv-1-referee -d "
            "--cap-drop ALL --security-opt no-new-privileges --read-only "
            "--tmpfs /tmp:exec --memory 4096m --pids-limit 512 "
            "--network saw-0-argv-1-net "
            "--mount type=volume,source=saw-0-argv-1-ws,target=/workspace "
            "--mount type=volume,source=saw-0-argv-1-referee-scratch,"
            "target=/sandbox "
            "--mount type=volume,source=arena-slot0-output_base,"
            "target=/output_base,readonly "
            "--entrypoint /bin/sh fake-image:1");
}

}  // namespace
}  // namespace tournament_arena
