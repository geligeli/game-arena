// A strategy is built once: the scheduler's build order runs on this host's
// daemon with this problem's sandbox image, its binaries go to the archive,
// and the placement's matches play them in other slots without building.
// Manual and local; the daemon needs the image:
//
//   bazel run //:sandbox_image_load   # once
//   bazel test //it:build_once_test

#include <grpcpp/grpcpp.h>
#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "game_arena/sandbox/exec/container_engine.h"
#include "game_arena/sandbox/worker/artifact_cache.h"
#include "game_arena/sandbox/worker/order_runner.h"
#include "game_arena/server/artifact_store.h"
#include "game_arena/server/candidate_store.h"
#include "game_arena/server/fleet_service.h"
#include "game_arena/server/fleet_worker.h"
#include "game_arena/server/problem_config.h"
#include "game_arena/server/scheduler.h"
#include "gtest/gtest.h"

namespace tournament_arena {
namespace {

class CapturingWorker : public FleetWorker {
 public:
  std::string worker_id() const override { return "it"; }
  int slots() const override { return 8; }
  bool builds_artifacts() const override { return true; }
  bool Send(const proto::FleetMessage &message) override {
    if (message.has_order()) {
      orders.push_back(message.order());
    }
    return true;
  }
  std::vector<proto::WorkOrder> orders;
};

std::string Read(const std::filesystem::path &path) {
  std::ifstream in(path);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

TEST(BuildOnceTest, BuiltOnceThenPlayedFromTheArchiveInOtherSlots) {
  const std::filesystem::path runfiles =
      std::filesystem::path(std::getenv("TEST_SRCDIR")) / "_main";
  const std::filesystem::path tmp = std::getenv("TEST_TMPDIR");
  std::string error;
  const auto problem =
      LoadProblemConfig(runfiles / "problem.textproto", &error);
  ASSERT_TRUE(problem.has_value()) << error;

  // The starter, submitted as the coordinator would store it.
  CandidateStore store(tmp / "candidates", CandidateLimits{},
                       problem->submission());
  proto::SubmitRequest request;
  request.set_display_name("it");
  request.set_game(problem->match().game());
  request.set_entry_header("strategy.h");
  auto *file = request.add_files();
  file->set_path("strategy.h");
  file->set_content(Read(runfiles / "bots/reference/strategy.h"));
  const auto candidate = store.Create(request, &error);
  ASSERT_TRUE(candidate.has_value()) << error;

  // The coordinator: a scheduler with an archive, served to the worker.
  ArtifactStore archive(tmp / "archive");
  Scheduler scheduler(SchedulerConfigFor(*problem), &store,
                      /*standings=*/nullptr, nullptr, nullptr, {}, &archive);
  FleetService fleet(&scheduler, &archive);
  grpc::ServerBuilder builder;
  int port = 0;
  builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(),
                           &port);
  builder.RegisterService(&fleet);
  const std::unique_ptr<grpc::Server> server = builder.BuildAndStart();
  auto worker = std::make_shared<CapturingWorker>();
  scheduler.AddWorker(worker);
  auto reservation =
      scheduler.TryReserve("", {}, /*cancel_running=*/false, &error);
  scheduler.EnqueuePlacement(*candidate, std::move(*reservation));
  ASSERT_EQ(worker->orders.size(), 1u);
  ASSERT_TRUE(worker->orders[0].build_only());

  // The worker, on this host's daemon.
  const auto stub = proto::SandboxFleet::NewStub(grpc::CreateChannel(
      "127.0.0.1:" + std::to_string(port), grpc::InsecureChannelCredentials()));
  ArtifactCache cache(tmp / "artifacts", stub.get());
  const std::string prefix = "connect4-it-" + std::to_string(::getpid());
  OrderJobConfig config;
  config.work_dir = tmp / "work";
  config.volume_prefix = prefix;
  sandbox_exec::ContainerEngine engine({});
  OrderRunner runner(/*process_engine=*/nullptr, &engine, config, "", &cache);
  ASSERT_TRUE(runner.Warmup(3, &error)) << error;

  const OrderOutcome built = runner.RunOrder(0, worker->orders[0], {});
  ASSERT_TRUE(built.result.build_ok()) << built.result.DebugString();
  ASSERT_EQ(built.result.artifacts().size(), 2u) << built.result.DebugString();
  proto::OrderResult result = built.result;  // as sandbox_worker sends it
  result.set_order_id(worker->orders[0].order_id());
  scheduler.OnResult("it", result);
  EXPECT_FALSE(store.Get(candidate->candidate_id())->artifact().empty());

  // Every placement match plays the archived binaries, each in a slot that
  // never built anything.
  ASSERT_EQ(worker->orders.size(), 3u);
  for (int slot : {1, 2}) {
    const proto::WorkOrder &match = worker->orders[slot];
    ASSERT_FALSE(match.candidate().artifact().empty());
    ASSERT_FALSE(match.referee_artifact().empty());
    const OrderOutcome played = runner.RunOrder(slot, match, {});
    EXPECT_EQ(played.result.build_output(), "ran the archived build\n");
    EXPECT_EQ(played.result.error(), "") << played.result.DebugString();
    EXPECT_EQ(played.result.games_played(), match.num_games());
  }

  server->Shutdown();
  // The caches are this test's own.
  EXPECT_EQ(std::system(("docker volume ls -q --filter name=" + prefix +
                         " | xargs -r docker volume rm >/dev/null")
                            .c_str()),
            0);
}

}  // namespace
}  // namespace tournament_arena
