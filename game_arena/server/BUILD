load("@rules_cc//cc:cc_binary.bzl", "cc_binary")

load("@rules_cc//cc:cc_library.bzl", "cc_library")

load("@rules_cc//cc:cc_test.bzl", "cc_test")
load("@rules_shell//shell:sh_test.bzl", "sh_test")

package(
    default_visibility = ["//visibility:public"],
)

# The broker process: gRPC services, the game/session domain, matchmaking,
# scheduling, ELO and history stores, and the HTTP leaderboard. Client-side
# strategy code lives in //game_arena/client.

# The BUILD file a structured submission is compiled with. Runs at submit time,
# because the generated BUILD is part of the patch that gets stored.
cc_library(
    name = "generated_build",
    srcs = ["generated_build.cc"],
    hdrs = ["generated_build.h"],
    deps = ["//game_arena/proto:problem_cc_proto"],
)

cc_test(
    name = "generated_build_test",
    srcs = ["generated_build_test.cc"],
    deps = [
        ":generated_build",
        "@googletest//:gtest_main",
    ],
)

# Who may submit, and how much. Holds hashes, never tokens.
cc_library(
    name = "client_registry",
    srcs = ["client_registry.cc"],
    hdrs = ["client_registry.h"],
    deps = [
        "//game_arena/proto:clients_cc_proto",
        "//game_arena/proto:problem_cc_proto",
        "@abseil-cpp//absl/log",
        "@abseil-cpp//absl/strings",
        "@boringssl//:crypto",
        "@protobuf//:protobuf",
    ],
)

cc_test(
    name = "client_registry_test",
    srcs = ["client_registry_test.cc"],
    deps = [
        ":client_registry",
        "@googletest//:gtest_main",
    ],
)

# How a problem's submissions are scored and ordered. Two problems ask
# genuinely different questions -- "who beats whom" and "how fast is it" -- and
# everything above this sees only rows.
cc_library(
    name = "standings",
    hdrs = ["standings.h"],
    deps = ["//game_arena/proto:arena_cc_proto"],
)

cc_library(
    name = "elo_standings",
    srcs = ["elo_standings.cc"],
    hdrs = ["elo_standings.h"],
    deps = [
        ":candidate_store",
        ":elo_store",
        ":standings",
        "@abseil-cpp//absl/log",
    ],
)

cc_library(
    name = "metric_standings",
    srcs = ["metric_standings.cc"],
    hdrs = ["metric_standings.h"],
    deps = [
        ":standings",
        "@abseil-cpp//absl/log",
    ],
)

cc_test(
    name = "standings_test",
    srcs = ["standings_test.cc"],
    deps = [
        ":elo_standings",
        ":metric_standings",
        "@googletest//:gtest_main",
    ],
)

# Reading and writing unified diffs: which paths a submission touches, how big
# it is, and what a newly added file contains. Header-level only -- applying a
# patch is the worker's job, in a container.
cc_library(
    name = "unified_diff",
    srcs = ["unified_diff.cc"],
    hdrs = ["unified_diff.h"],
    deps = ["@abseil-cpp//absl/strings"],
)

cc_test(
    name = "unified_diff_test",
    srcs = ["unified_diff_test.cc"],
    deps = [
        ":unified_diff",
        "@googletest//:gtest_main",
    ],
)

# What a problem is, as data: parsing, defaulting and checking the .textproto
# the server is started with. No problem-specific code -- just the schema.
cc_library(
    name = "problem_config",
    srcs = ["problem_config.cc"],
    hdrs = ["problem_config.h"],
    deps = [
        "//game_arena/common/kv_options",
        "//game_arena/proto:problem_cc_proto",
        "@abseil-cpp//absl/strings",
        "@protobuf//:protobuf",
    ],
)

cc_test(
    name = "problem_config_test",
    srcs = ["problem_config_test.cc"],
    deps = [
        ":problem_config",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "shipped_problems_test",
    srcs = ["shipped_problems_test.cc"],
    data = ["//game_arena/problems:configs"],
    deps = [
        ":problem_config",
        "@googletest//:gtest_main",
    ],
)

# Persistent registry of submitted strategies: manifests plus their sources on
# disk, and the validation gate every submission passes through.
cc_library(
    name = "candidate_store",
    srcs = ["candidate_store.cc"],
    hdrs = ["candidate_store.h"],
    deps = [
        ":generated_build",
        ":unified_diff",
        "//game_arena/proto:arena_cc_proto",
        "//game_arena/proto:problem_cc_proto",
        "@abseil-cpp//absl/log",
    ],
)

cc_test(
    name = "candidate_store_test",
    srcs = ["candidate_store_test.cc"],
    deps = [
        ":candidate_store",
        "@googletest//:gtest_main",
    ],
)

# One attached sandbox worker, as the scheduler sees it. Kept gRPC-free so the
# scheduler can be tested with fakes.
cc_library(
    name = "fleet_worker",
    hdrs = ["fleet_worker.h"],
    deps = ["//game_arena/proto:arena_cc_proto"],
)

# Expands "rate this candidate" into work orders and hands them to the fleet.
cc_library(
    name = "scheduler",
    srcs = ["scheduler.cc"],
    hdrs = ["scheduler.h"],
    deps = [
        ":candidate_store",
        ":elo_store",
        ":fleet_worker",
        ":problem_config",
        ":standings",
        "//game_arena/proto:arena_cc_proto",
        "//game_arena/proto:clients_cc_proto",
        "@abseil-cpp//absl/log",
    ],
)

cc_test(
    name = "scheduler_test",
    srcs = ["scheduler_test.cc"],
    deps = [
        ":elo_standings",
        ":scheduler",
        "@googletest//:gtest_main",
    ],
)

cc_library(
    name = "elo_store",
    srcs = ["elo_store.cc"],
    hdrs = ["elo_store.h"],
    deps = [
        "//game_arena/proto:tournament_broker_cc_proto",
        "@abseil-cpp//absl/log",
    ],
)

cc_library(
    name = "game_history",
    srcs = ["game_history.cc"],
    hdrs = ["game_history.h"],
    deps = [
        "//game_arena/proto:tournament_broker_cc_proto",
        "@abseil-cpp//absl/log",
    ],
)

# Agent-facing arena RPCs, consumed by the MCP server.
cc_library(
    name = "arena_service",
    srcs = ["arena_service.cc"],
    hdrs = ["arena_service.h"],
    deps = [
        ":candidate_store",
        ":client_registry",
        ":elo_store",
        ":scheduler",
        "//game_arena/proto:arena_cc_grpc",
        "@abseil-cpp//absl/log",
        "@grpc//:grpc++",
    ],
)

# Sandbox fleet RPCs: one Attach stream per worker host.
cc_library(
    name = "fleet_service",
    srcs = ["fleet_service.cc"],
    hdrs = ["fleet_service.h"],
    deps = [
        ":fleet_worker",
        ":scheduler",
        "//game_arena/proto:arena_cc_grpc",
        "@abseil-cpp//absl/log",
        "@grpc//:grpc++",
    ],
)

cc_library(
    name = "http_leaderboard",
    srcs = ["http_leaderboard.cc"],
    hdrs = ["http_leaderboard.h"],
    deps = [
        ":candidate_store",
        ":game_history",
        ":standings",
        "@abseil-cpp//absl/log",
    ],
)

# The coordinator. Links no game and no problem code -- enforced by
# :no_problem_code_test, not by convention.
cc_binary(
    name = "problem_server",
    srcs = ["main.cc"],
    deps = [
        ":arena_service",
        ":candidate_store",
        ":elo_standings",
        ":elo_store",
        ":fleet_service",
        ":game_history",
        ":http_leaderboard",
        ":metric_standings",
        ":problem_config",
        ":scheduler",
        ":standings",
        "@abseil-cpp//absl/flags:flag",
        "@abseil-cpp//absl/flags:parse",
        "@abseil-cpp//absl/log",
        "@abseil-cpp//absl/log:globals",
        "@abseil-cpp//absl/log:initialize",
        "@grpc//:grpc++",
    ],
)

# The coordinator must never link a game.
#
# Checked on the linked binary rather than on the dependency graph, because the
# graph has more than one route in: the referee package, a game target directly,
# or something that quietly grew a dep on one. Symbols do not care which route
# was taken.
sh_test(
    name = "no_problem_code_test",
    srcs = ["no_problem_code_test.sh"],
    data = [":problem_server"],
)

cc_test(
    name = "elo_store_test",
    srcs = ["elo_store_test.cc"],
    deps = [
        ":elo_store",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "arena_integration_test",
    srcs = ["arena_integration_test.cc"],
    deps = [
        ":elo_standings",
        ":arena_service",
        ":candidate_store",
        ":elo_store",
        ":fleet_service",
        ":scheduler",
        "//game_arena/proto:arena_cc_grpc",
        "@googletest//:gtest_main",
        "@grpc//:grpc++",
    ],
)
