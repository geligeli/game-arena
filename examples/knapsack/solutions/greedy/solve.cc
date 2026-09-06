// A second submission, so the leaderboard has something to order.
//
// Takes items by value-per-weight until nothing else fits. Fast, obvious, and
// not optimal -- which is the point: it should score below the reference and
// give a submitter something to beat.

#include <algorithm>
#include <fstream>
#include <iostream>
#include <numeric>
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

  std::vector<int> order(n);
  std::iota(order.begin(), order.end(), 0);
  std::sort(order.begin(), order.end(), [&](int a, int b) {
    return static_cast<double>(value[a]) / weight[a] >
           static_cast<double>(value[b]) / weight[b];
  });

  int remaining = capacity;
  for (const int i : order) {
    if (weight[i] <= remaining) {
      remaining -= weight[i];
      std::cout << i << "\n";
    }
  }
  return 0;
}
