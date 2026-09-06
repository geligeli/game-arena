#include "game_arena/common/process/process.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>

namespace process {
namespace {

auto MakeTempFilePath(const std::string& suffix) -> std::filesystem::path {
  static std::atomic<uint64_t> counter{0};
  auto name =
      std::string("process_test_") + suffix + "_" + std::to_string(counter++);
  return std::filesystem::temp_directory_path() / name;
}

auto ReadFile(const std::filesystem::path& path) -> std::string {
  std::ifstream file(path);
  if (!file) {
    throw std::runtime_error("failed to open file: " + path.string());
  }
  std::ostringstream buffer;
  buffer << file.rdbuf();
  return buffer.str();
}

}  // namespace

TEST(ProcessTest, PipesInputToChildStdout) {
  const auto stdout_path = MakeTempFilePath("stdout");
  auto process = CreateInputStreamProcess(
      "/usr/bin/python3",
      {"-c",
       "import sys; data = sys.stdin.read(); sys.stdout.write(data.upper())"},
      {}, stdout_path);

  process.stdin() << "risk\n";
  EXPECT_EQ(process.Wait(), 0);

  EXPECT_EQ(ReadFile(stdout_path), "RISK\n");
  std::filesystem::remove(stdout_path);
}

TEST(ProcessTest, CapturesStderrAndExitCode) {
  const auto stderr_path = MakeTempFilePath("stderr");
  auto process = CreateInputStreamProcess(
      "/usr/bin/python3",
      {"-c", "import sys; sys.stderr.write('oops\\n'); sys.exit(3)"}, {},
      "/dev/null", stderr_path);

  EXPECT_EQ(process.Wait(), 3);
  EXPECT_EQ(ReadFile(stderr_path), "oops\n");
  std::filesystem::remove(stderr_path);
}

TEST(ProcessTest, SupportsMoveSemantics) {
  const auto stdout_path = MakeTempFilePath("move");
  InputStreamProcess source = CreateInputStreamProcess(
      "/usr/bin/python3",
      {"-c", "import sys; sys.stdout.write(sys.stdin.read())"}, {},
      stdout_path);

  InputStreamProcess target = std::move(source);
  target.stdin() << "abc";
  EXPECT_EQ(target.Wait(), 0);
  EXPECT_EQ(ReadFile(stdout_path), "abc");
  std::filesystem::remove(stdout_path);
}

// --- RunCommand ------------------------------------------------------------

TEST(RunCommandTest, CapturesOutputAndExitCode) {
  const auto out = MakeTempFilePath("run_out");
  const auto err = MakeTempFilePath("run_err");
  RunOptions options;
  options.stdout_path = out;
  options.stderr_path = err;

  const RunResult result = RunCommand(
      "sh", {"-c", "echo to-stdout; echo to-stderr >&2; exit 3"}, options);
  EXPECT_TRUE(result.started);
  EXPECT_FALSE(result.timed_out);
  EXPECT_EQ(result.exit_code, 3);
  EXPECT_EQ(ReadFile(out), "to-stdout\n");
  EXPECT_EQ(ReadFile(err), "to-stderr\n");
  std::filesystem::remove(out);
  std::filesystem::remove(err);
}

TEST(RunCommandTest, RunsInTheRequestedDirectory) {
  const auto out = MakeTempFilePath("run_cwd");
  RunOptions options;
  options.cwd = "/tmp";
  options.stdout_path = out;

  const RunResult result = RunCommand("sh", {"-c", "pwd"}, options);
  ASSERT_TRUE(result.started);
  EXPECT_EQ(result.exit_code, 0);
  EXPECT_EQ(ReadFile(out), "/tmp\n");
  std::filesystem::remove(out);
}

TEST(RunCommandTest, PassesAnExplicitEnvironment) {
  const auto out = MakeTempFilePath("run_env");
  RunOptions options;
  options.env = {"MARKER=hello", "PATH=/bin:/usr/bin"};
  options.stdout_path = out;

  const RunResult result = RunCommand("sh", {"-c", "echo $MARKER"}, options);
  ASSERT_TRUE(result.started);
  EXPECT_EQ(ReadFile(out), "hello\n");
  std::filesystem::remove(out);
}

TEST(RunCommandTest, ReportsAMissingExecutableWithoutThrowing) {
  const RunResult result =
      RunCommand("definitely-not-a-real-binary-xyz", {}, RunOptions{});
  EXPECT_FALSE(result.started);
  EXPECT_FALSE(result.timed_out);
}

// A build tool that hangs must not hang the worker with it.
TEST(RunCommandTest, KillsAChildThatOverrunsItsTimeout) {
  RunOptions options;
  options.timeout = std::chrono::seconds(1);
  options.kill_grace = std::chrono::seconds(1);

  const auto start = std::chrono::steady_clock::now();
  const RunResult result = RunCommand("sleep", {"120"}, options);
  const auto elapsed = std::chrono::steady_clock::now() - start;

  EXPECT_TRUE(result.started);
  EXPECT_TRUE(result.timed_out);
  EXPECT_LT(std::chrono::duration_cast<std::chrono::seconds>(elapsed).count(),
            15);
}

// The child gets its own process group precisely so a timeout reaches the
// grandchildren too: build tools fan out into workers that would otherwise
// survive their parent.
TEST(RunCommandTest, TimeoutReachesTheWholeProcessGroup) {
  const auto marker = MakeTempFilePath("group_marker");
  RunOptions options;
  options.timeout = std::chrono::seconds(1);
  options.kill_grace = std::chrono::seconds(1);

  // The grandchild would create the marker after 8s if it survived the kill.
  const RunResult result = RunCommand(
      "sh", {"-c", "sh -c 'sleep 8; touch " + marker.string() + "' & wait"},
      options);
  ASSERT_TRUE(result.started);
  EXPECT_TRUE(result.timed_out);

  std::this_thread::sleep_for(std::chrono::seconds(10));
  EXPECT_FALSE(std::filesystem::exists(marker))
      << "grandchild outlived the process-group kill";
  std::filesystem::remove(marker);
}

TEST(RunCommandTest, AddressSpaceLimitStopsARunawayAllocation) {
  RunOptions options;
  options.address_space_limit_bytes = 64u * 1024 * 1024;
  options.timeout = std::chrono::seconds(30);

  // Well past the cap, so the allocation must fail rather than succeed slowly.
  const RunResult result = RunCommand(
      "python3", {"-c", "b = bytearray(512 * 1024 * 1024); print(len(b))"},
      options);
  ASSERT_TRUE(result.started);
  EXPECT_FALSE(result.timed_out);
  EXPECT_NE(result.exit_code, 0) << "the cap did not bind";
}

TEST(ResolveExecutableTest, FindsOnPathAndValidatesExplicitPaths) {
  const std::string sh = ResolveExecutable("sh");
  EXPECT_FALSE(sh.empty());
  EXPECT_EQ(sh.front(), '/');

  EXPECT_TRUE(ResolveExecutable("definitely-not-a-real-binary-xyz").empty());
  EXPECT_TRUE(ResolveExecutable("").empty());
  // An explicit path is used as given, and rejected when not executable.
  EXPECT_EQ(ResolveExecutable("/bin/sh"), "/bin/sh");
  EXPECT_TRUE(ResolveExecutable("/nonexistent/binary").empty());
}

}  // namespace process