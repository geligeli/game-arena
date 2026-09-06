// The grader: runs a submission over every case, checks it, and reports a
// score.
//
// This is the whole of what a graded problem has to provide. The arena builds
// the submission, runs this command in the sandbox, and reads the numbers back
// from $ARENA_REPORT; how those numbers are arrived at is entirely here.
//
// Two rules worth copying into any grader:
//
//   - Check feasibility yourself. A submission is not a collaborator, and the
//     score has to be something it cannot claim without earning.
//   - A submission that crashes, hangs or answers nonsense scores zero for that
//     case and the run continues. Only the grader failing is a failed run --
//     otherwise one bad case looks like a broken problem.

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "absl/flags/flag.h"
#include "absl/flags/parse.h"
#include "game_arena/common/process/process.h"
#include "game_arena/grader/report.h"

ABSL_FLAG(std::string, solution, "",
          "Executable to grade: takes an instance path as its one argument "
          "and writes chosen item indices on stdout (required)");
ABSL_FLAG(std::string, cases, "cases", "Directory of <name>.txt instances");
ABSL_FLAG(int, per_case_timeout_s, 10, "Wall-clock limit for one case");

namespace {

struct Instance {
  int capacity = 0;
  std::vector<int> value;
  std::vector<int> weight;
};

auto ReadInstance(const std::filesystem::path &path, Instance *out) -> bool {
  std::ifstream in(path);
  int n = 0;
  if (!(in >> n >> out->capacity) || n < 0) {
    return false;
  }
  out->value.resize(n);
  out->weight.resize(n);
  for (int i = 0; i < n; ++i) {
    if (!(in >> out->value[i] >> out->weight[i])) {
      return false;
    }
  }
  return true;
}

auto ReadBest(const std::filesystem::path &path, int *best) -> bool {
  std::ifstream in(path);
  return static_cast<bool>(in >> *best) && *best > 0;
}

// Scores one answer, or returns nullopt if it is not a legal packing. The
// distinction matters: an infeasible answer is not a low score, it is no score,
// and a submitter is better served by being told which.
auto ScoreAnswer(const Instance &instance, const std::string &answer,
                 std::string *why) -> std::optional<int> {
  std::istringstream in(answer);
  std::set<int> chosen;
  int index = 0;
  while (in >> index) {
    if (index < 0 || index >= static_cast<int>(instance.value.size())) {
      *why = "item index " + std::to_string(index) + " is out of range";
      return std::nullopt;
    }
    if (!chosen.insert(index).second) {
      *why = "item " + std::to_string(index) + " chosen twice";
      return std::nullopt;
    }
  }
  if (!in.eof()) {
    *why = "output is not a whitespace-separated list of integers";
    return std::nullopt;
  }
  int weight = 0;
  int value = 0;
  for (const int i : chosen) {
    weight += instance.weight[i];
    value += instance.value[i];
  }
  if (weight > instance.capacity) {
    *why = "weight " + std::to_string(weight) + " exceeds capacity " +
           std::to_string(instance.capacity);
    return std::nullopt;
  }
  return value;
}

auto ReadFile(const std::filesystem::path &path) -> std::string {
  std::ifstream in(path, std::ios::binary);
  std::ostringstream out;
  out << in.rdbuf();
  return out.str();
}

}  // namespace

auto main(int argc, char **argv) -> int {
  absl::ParseCommandLine(argc, argv);

  const std::string solution = absl::GetFlag(FLAGS_solution);
  if (solution.empty()) {
    std::cerr << "--solution is required\n";
    return 2;
  }
  if (!std::filesystem::exists(solution)) {
    std::cerr << "no such solution binary: " << solution << "\n";
    return 2;
  }

  std::vector<std::filesystem::path> cases;
  for (const auto &entry :
       std::filesystem::directory_iterator(absl::GetFlag(FLAGS_cases))) {
    if (entry.path().extension() == ".txt") {
      cases.push_back(entry.path());
    }
  }
  std::sort(cases.begin(), cases.end());
  if (cases.empty()) {
    std::cerr << "no cases found in " << absl::GetFlag(FLAGS_cases) << "\n";
    return 2;
  }

  const auto scratch =
      std::filesystem::temp_directory_path() / "knapsack_grade";
  std::filesystem::create_directories(scratch);

  double total_ratio = 0.0;
  int solved = 0;
  int infeasible = 0;
  for (const std::filesystem::path &path : cases) {
    Instance instance;
    int best = 0;
    if (!ReadInstance(path, &instance) ||
        !ReadBest(path.parent_path() / (path.stem().string() + ".best"),
                  &best)) {
      std::cerr << "the problem's own case " << path
                << " is unreadable; this is a broken problem, not a bad "
                   "submission\n";
      return 2;  // a grader that cannot read its own cases must not score
    }

    const auto out_path = scratch / (path.stem().string() + ".out");
    process::RunOptions options;
    options.stdout_path = out_path;
    options.stderr_path = scratch / (path.stem().string() + ".err");
    options.timeout =
        std::chrono::seconds(absl::GetFlag(FLAGS_per_case_timeout_s));
    // Without a cap, a submission that allocates without bound takes the whole
    // worker down with it rather than failing its own case.
    options.address_space_limit_bytes = std::size_t{2} << 30;

    const process::RunResult result =
        process::RunCommand(solution, {path.string()}, options);
    if (!result.started || result.timed_out || result.exit_code != 0) {
      std::cerr << path.stem().string() << ": no answer ("
                << (result.timed_out
                        ? "timed out"
                        : "exit " + std::to_string(result.exit_code))
                << ")\n";
      continue;
    }

    std::string why;
    const std::optional<int> value =
        ScoreAnswer(instance, ReadFile(out_path), &why);
    if (!value.has_value()) {
      std::cerr << path.stem().string() << ": infeasible -- " << why << "\n";
      ++infeasible;
      continue;
    }
    ++solved;
    const double ratio = static_cast<double>(*value) / best;
    total_ratio += ratio;
    std::cerr << path.stem().string() << ": value " << *value << " of " << best
              << " (" << (100.0 * ratio) << "%)\n";
  }

  const double score = 100.0 * total_ratio / static_cast<double>(cases.size());
  std::map<std::string, double> metrics{
      {"score", score},
      {"solved", solved},
      {"infeasible", infeasible},
      {"cases", static_cast<double>(cases.size())},
  };

  std::string error;
  if (!grader::WriteReportToArenaPath(metrics, &error)) {
    // Outside the arena there is nowhere to put the report, which is normal
    // when running this by hand. Print it and succeed.
    std::cerr << error << "\n";
    std::cout << grader::RenderReport(metrics);
    return 0;
  }
  std::cerr << "score " << score << "\n";
  return 0;
}
