load("@rules_cc//cc:cc_library.bzl", "cc_library")

package(default_visibility = ["//visibility:public"])

# The scratch candidate. This is the same shape the sandbox worker generates
# for a submission, so a strategy that builds here builds in the arena.
cc_library(
    name = "strategy",
    hdrs = ["strategy.h"],
    deps = ["//game_mcts/tournament_server/candidate:candidate_api"],
)
