// A worker slot builds each version once: the orders the scheduler makes of
// a placement, run in one slot against this host's daemon and this problem's
// sandbox image. Manual and local; the daemon needs the image:
//
//   bazel run //:sandbox_image_load   # once
//   bazel test //it:build_once_test

#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "game_arena/sandbox/exec/container_engine.h"
#include "game_arena/sandbox/worker/order_runner.h"
#include "game_arena/server/candidate_store.h"
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

TEST(BuildOnceTest, ASlotBuildsAVersionOnceAndPlaysItAgainWithoutABuild) {
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

  // Placement: one order per builtin, the same code in each.
  Scheduler scheduler(SchedulerConfigFor(*problem), &store,
                      /*standings=*/nullptr);
  auto worker = std::make_shared<CapturingWorker>();
  scheduler.AddWorker(worker);
  auto reservation =
      scheduler.TryReserve("", {}, /*cancel_running=*/false, &error);
  scheduler.EnqueuePlacement(*candidate, std::move(*reservation));
  ASSERT_GE(worker->orders.size(), 2u);

  const std::string prefix = "connect4-it-" + std::to_string(::getpid());
  OrderJobConfig config;
  config.work_dir = tmp / "work";
  config.disk_cache = tmp / "work" / "disk_cache";
  config.volume_prefix = prefix;
  sandbox_exec::ContainerEngine engine({});
  OrderRunner runner(/*process_engine=*/nullptr, &engine, config);
  ASSERT_TRUE(runner.Warmup(1, &error)) << error;

  const OrderOutcome first = runner.RunOrder(0, worker->orders[0], {});
  ASSERT_TRUE(first.result.build_ok()) << first.result.DebugString();
  EXPECT_EQ(first.result.games_played(), worker->orders[0].num_games());

  const OrderOutcome second = runner.RunOrder(0, worker->orders[1], {});
  EXPECT_TRUE(second.result.build_ok()) << second.result.DebugString();
  EXPECT_EQ(second.result.build_output(), "reused this slot's earlier build\n");
  EXPECT_EQ(second.result.games_played(), worker->orders[1].num_games())
      << second.result.DebugString();

  // The caches are this test's own.
  EXPECT_EQ(std::system(("docker volume ls -q --filter name=" + prefix +
                         " | xargs -r docker volume rm >/dev/null")
                            .c_str()),
            0);
}

}  // namespace
}  // namespace tournament_arena
