// A graded problem builds and grades in its one order, as it always has: the
// grader runs bazel-bin/... from the tree its build linked. Manual and local;
// the daemon needs the image:
//
//   bazel run //:sandbox_image_load   # once
//   bazel test //it:grade_test

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
  int slots() const override { return 1; }
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

TEST(GradeTest, TheReferenceSolutionIsBuiltAndScored) {
  const std::filesystem::path runfiles =
      std::filesystem::path(std::getenv("TEST_SRCDIR")) / "_main";
  const std::filesystem::path tmp = std::getenv("TEST_TMPDIR");
  std::string error;
  const auto problem =
      LoadProblemConfig(runfiles / "problem.textproto", &error);
  ASSERT_TRUE(problem.has_value()) << error;

  CandidateStore store(tmp / "candidates", CandidateLimits{},
                       problem->submission());
  proto::SubmitRequest request;
  request.set_display_name("it");
  request.set_entry_header("solve.cc");
  auto *file = request.add_files();
  file->set_path("solve.cc");
  file->set_content(Read(runfiles / "solutions/reference/solve.cc"));
  const auto candidate = store.Create(request, &error);
  ASSERT_TRUE(candidate.has_value()) << error;

  Scheduler scheduler(SchedulerConfigFor(*problem), &store,
                      /*standings=*/nullptr);
  auto worker = std::make_shared<CapturingWorker>();
  scheduler.AddWorker(worker);
  auto reservation =
      scheduler.TryReserve("", {}, /*cancel_running=*/false, &error);
  scheduler.EnqueuePlacement(*candidate, std::move(*reservation));
  ASSERT_EQ(worker->orders.size(), 1u);
  ASSERT_TRUE(worker->orders[0].has_grade());

  const std::string prefix = "knapsack-it-" + std::to_string(::getpid());
  OrderJobConfig config;
  config.work_dir = tmp / "work";
  config.volume_prefix = prefix;
  sandbox_exec::ContainerEngine engine({});
  OrderRunner runner(/*process_engine=*/nullptr, &engine, config);
  ASSERT_TRUE(runner.Warmup(1, &error)) << error;

  const OrderOutcome graded = runner.RunOrder(0, worker->orders[0], {});
  EXPECT_TRUE(graded.result.build_ok()) << graded.result.DebugString();
  EXPECT_EQ(graded.result.error(), "") << graded.result.DebugString();
  EXPECT_TRUE(graded.result.metrics().contains("score"))
      << graded.result.DebugString();

  EXPECT_EQ(std::system(("docker volume ls -q --filter name=" + prefix +
                         " | xargs -r docker volume rm >/dev/null")
                            .c_str()),
            0);
}

}  // namespace
}  // namespace tournament_arena
