load("@rules_cc//cc:cc_test.bzl", "cc_test")

package(default_visibility = ["//visibility:public"])

# Against this host's daemon and the problem's sandbox image: manual, local
# and never cached.
cc_test(
    name = "grade_test",
    srcs = ["grade_test.cc"],
    data = [
        "//:problem.textproto",
        "//solutions/reference:kit",
    ],
    tags = [
        "external",
        "local",
        "manual",
    ],
    deps = [
        "@game_arena//game_arena/sandbox/exec:container_engine",
        "@game_arena//game_arena/sandbox/worker:order_runner",
        "@game_arena//game_arena/server:candidate_store",
        "@game_arena//game_arena/server:fleet_worker",
        "@game_arena//game_arena/server:problem_config",
        "@game_arena//game_arena/server:scheduler",
        "@googletest//:gtest_main",
    ],
)
