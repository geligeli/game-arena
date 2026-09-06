// The reference solution, and the shape a submission takes.
//
// Reads the instance named by argv[1] and prints the indices of the items it
// packs. This one is exact (a textbook DP), so it scores 100 and exists to show
// the contract rather than to be beaten.

#include <fstream>
#include <iostream>
#include <vector>

auto main(int argc, char **argv) -> int {
  if (argc < 2) {
    std::cerr << "usage: solve <instance>\n";
    return 2;
  }
  std::ifstream in(argv[1]);
  int n = 0;
  int capacity = 0;
  if (!(in >> n >> capacity)) {
    std::cerr << "cannot read the instance header\n";
    return 2;
  }
  std::vector<int> value(n);
  std::vector<int> weight(n);
  for (int i = 0; i < n; ++i) {
    in >> value[i] >> weight[i];
  }

  // best[i][c] = best value using the first i items within capacity c.
  std::vector<std::vector<int>> best(n + 1, std::vector<int>(capacity + 1, 0));
  for (int i = 1; i <= n; ++i) {
    for (int c = 0; c <= capacity; ++c) {
      best[i][c] = best[i - 1][c];
      if (weight[i - 1] <= c) {
        best[i][c] =
            std::max(best[i][c], best[i - 1][c - weight[i - 1]] + value[i - 1]);
      }
    }
  }

  // Walk the table back to recover which items were taken.
  int c = capacity;
  for (int i = n; i > 0; --i) {
    if (best[i][c] != best[i - 1][c]) {
      std::cout << (i - 1) << "\n";
      c -= weight[i - 1];
    }
  }
  return 0;
}
