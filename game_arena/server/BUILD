load("@rules_cc//cc:cc_binary.bzl", "cc_binary")
load("@rules_cc//cc:cc_library.bzl", "cc_library")
load("@rules_cc//cc:cc_test.bzl", "cc_test")
load("@protobuf//bazel:cc_proto_library.bzl", "cc_proto_library")
load("@protobuf//bazel:proto_library.bzl", "proto_library")
load("@rules_shell//shell:sh_test.bzl", "sh_test")

package(
    default_visibility = ["//visibility:public"],
)

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

cc_library(
    name = "candidate_store",
    srcs = ["candidate_store.cc"],
    hdrs = ["candidate_store.h"],
    deps = [
        "//game_arena/standings:candidate_view",
        ":generated_build",
        ":problem_config",
        ":unified_diff",
        "//game_arena/proto:arena_cc_proto",
        "//game_arena/proto:problem_cc_proto",
        "@abseil-cpp//absl/log",
        "@abseil-cpp//absl/strings",
        "@abseil-cpp//absl/time",
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

cc_library(
    name = "fleet_worker",
    hdrs = ["fleet_worker.h"],
    deps = ["//game_arena/proto:arena_cc_proto"],
)

proto_library(
    name = "job_record_proto",
    srcs = ["job_record.proto"],
    deps = ["//game_arena/proto:arena_proto"],
)

cc_proto_library(
    name = "job_record_cc_proto",
    deps = [":job_record_proto"],
)

cc_library(
    name = "job_log",
    srcs = ["job_log.cc"],
    hdrs = ["job_log.h"],
    deps = [
        ":job_record_cc_proto",
        "//game_arena/standings:game_history",
        "@abseil-cpp//absl/log",
    ],
)

cc_test(
    name = "job_log_test",
    srcs = ["job_log_test.cc"],
    deps = [
        ":job_log",
        "@googletest//:gtest_main",
    ],
)

cc_library(
    name = "dashboard",
    srcs = ["dashboard.cc"],
    hdrs = ["dashboard.h"],
    deps = [
        ":candidate_store",
        ":job_log",
        ":unified_diff",
        "//game_arena/standings:game_history",
        "//game_arena/standings:http_leaderboard",
        "//game_arena/standings:standings",
        "@abseil-cpp//absl/strings",
        "@abseil-cpp//absl/time",
        "@boost.json",
    ],
)

cc_test(
    name = "dashboard_test",
    srcs = ["dashboard_test.cc"],
    deps = [
        ":dashboard",
        "@googletest//:gtest_main",
    ],
)

proto_library(
    name = "scheduler_config_proto",
    srcs = ["scheduler_config.proto"],
    deps = ["//game_arena/proto:arena_proto"],
)

cc_proto_library(
    name = "scheduler_config_cc_proto",
    deps = [":scheduler_config_proto"],
)

cc_library(
    name = "scheduler",
    srcs = ["scheduler.cc"],
    hdrs = ["scheduler.h"],
    deps = [
        ":candidate_store",
        ":fleet_worker",
        ":job_log",
        ":problem_config",
        ":scheduler_config_cc_proto",
        "//game_arena/standings:game_history",
        "//game_arena/standings:standings",
        "//game_arena/proto:arena_cc_proto",
        "//game_arena/proto:tournament_broker_cc_proto",
        "//game_arena/proto:clients_cc_proto",
        "@abseil-cpp//absl/log",
        "@abseil-cpp//absl/time",
    ],
)

cc_test(
    name = "scheduler_test",
    srcs = ["scheduler_test.cc"],
    deps = [
        "//game_arena/standings:elo_standings",
        "//game_arena/standings:game_history",
        ":scheduler",
        "@googletest//:gtest_main",
    ],
)

cc_library(
    name = "arena_service",
    srcs = ["arena_service.cc"],
    hdrs = ["arena_service.h"],
    deps = [
        ":candidate_store",
        ":client_registry",
        ":scheduler",
        "//game_arena/proto:arena_cc_grpc",
        "@abseil-cpp//absl/log",
        "@grpc//:grpc++",
    ],
)

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

cc_binary(
    name = "problem_server",
    srcs = ["problem_server.cc"],
    deps = [
        ":arena_service",
        ":candidate_store",
        ":dashboard",
        "//game_arena/standings:elo_standings",
        "//game_arena/standings:elo_store",
        ":fleet_service",
        "//game_arena/standings:game_history",
        "//game_arena/standings:http_leaderboard",
        ":job_log",
        "//game_arena/standings:metric_standings",
        ":problem_config",
        ":scheduler",
        "//game_arena/standings:standings",
        "@abseil-cpp//absl/flags:flag",
        "@abseil-cpp//absl/flags:parse",
        "@abseil-cpp//absl/log",
        "@abseil-cpp//absl/log:globals",
        "@abseil-cpp//absl/log:initialize",
        "@grpc//:grpc++",
    ],
)

# On the linked binary's symbols: the dep graph has more than one route in.
sh_test(
    name = "no_problem_code_test",
    srcs = ["no_problem_code_test.sh"],
    data = [":problem_server"],
)

cc_test(
    name = "arena_integration_test",
    srcs = ["arena_integration_test.cc"],
    deps = [
        "//game_arena/standings:elo_standings",
        ":arena_service",
        ":candidate_store",
        "//game_arena/standings:elo_store",
        ":fleet_service",
        ":scheduler",
        "//game_arena/proto:arena_cc_grpc",
        "@googletest//:gtest_main",
        "@grpc//:grpc++",
    ],
)
