#include "game_arena/sandbox/worker/order_job.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "game_arena/common/kv_options/kv_options.h"
#include "game_arena/sandbox/common/docker.h"
#include "game_arena/sandbox/worker/bot_launch.h"

namespace tournament_arena {

namespace sx = sandbox_exec::proto;

namespace {

constexpr int kDefaultTimeoutS = 1800;
// The port a refereed match uses inside its own network namespace. Fixed
// rather than discovered because the namespace is private: nothing else is
// there to collide with.
constexpr int kMatchPort = 50051;

auto Verbatim(const std::string &text) -> sx::Token {
  sx::Token token;
  token.set_text(text);
  token.set_verbatim(true);
  return token;
}

auto Quoted(const std::string &text) -> sx::Token {
  sx::Token token;
  token.set_text(text);
  return token;
}

auto SlotDir(const OrderJobConfig &config, int slot) -> std::filesystem::path {
  return config.work_dir / ("slot" + std::to_string(slot));
}

auto Isolation(const OrderJobConfig &config, bool container) -> sx::Isolation {
  sx::Isolation isolation;
  if (!container) {
    // No image and no cgroups. The one limit this engine can apply is an
    // address-space cap, and it goes on the steps that run submitted code
    // rather than here -- see SolutionIsolation. A cap on the build would be
    // a cap on bazel, whose JVM reserves far more address space than any
    // limit a problem means for a solution, and it dies at startup.
    return isolation;
  }
  isolation.set_image(config.image);
  isolation.set_memory_limit_mb(config.memory_limit_mb);
  isolation.set_cpus(config.cpus);
  isolation.set_pids_limit(config.pids_limit);
  isolation.set_run_as_user(config.run_as_user);
  if (config.host_overlay) {
    // Nothing left to mount, so nothing to be privileged for: the defaults
    // (drop every capability, no new privileges, a read-only root) stand, and
    // /tmp is the one writable place.
    sx::Tmpfs *tmpfs = isolation.add_tmpfs();
    tmpfs->set_target("/tmp");
    tmpfs->set_options("exec");
  } else {
    // The in-sandbox overlay needs CAP_SYS_ADMIN and a writable root. This is
    // the mode ARENA.md says is not a boundary, expressed as exactly the
    // relaxations it costs.
    isolation.set_keep_default_caps(true);
    isolation.set_allow_new_privileges(true);
    isolation.set_writable_rootfs(true);
    isolation.add_add_capabilities("SYS_ADMIN");
  }
  return isolation;
}

// The isolation for a step that runs the submission itself: the problem's
// memory limit, as an address-space cap the process engine can enforce. Not
// applied to the build, for the reason in Isolation() above.
auto SolutionIsolation(const OrderJobConfig &config, bool container,
                       const sx::Isolation &base) -> sx::Isolation {
  sx::Isolation isolation = base;
  if (!container && config.memory_limit_mb > 0) {
    isolation.set_address_space_limit_bytes(
        static_cast<std::uint64_t>(config.memory_limit_mb) * 1024 * 1024);
  }
  return isolation;
}

// The output base and disk cache as the sandbox sees them, or as host paths
// when there is no sandbox.
struct BuildPaths {
  std::string output_base;
  std::string disk_cache;
  std::string bazel_bin;
};

auto PathsFor(const OrderJobConfig &config, int slot,
              bool container) -> BuildPaths {
  BuildPaths paths;
  if (container) {
    paths.output_base = sandbox_common::kOutputBaseMount;
    paths.disk_cache =
        config.disk_cache.empty() ? "" : sandbox_common::kDiskCacheMount;
    paths.bazel_bin = "./bazel-bin/";
    return paths;
  }
  paths.output_base = (SlotDir(config, slot) / "bazel_output_base").string();
  paths.disk_cache = config.disk_cache.string();
  paths.bazel_bin =
      (SlotDir(config, slot) / "repo" / "bazel-bin").string() + "/";
  return paths;
}

auto SidesOf(const proto::WorkOrder &order)
    -> std::vector<const proto::Side *> {
  std::vector<const proto::Side *> sides = {&order.candidate()};
  if (order.has_opponent()) {
    sides.push_back(&order.opponent());
  }
  return sides;
}

auto WorkspaceFor(const proto::WorkOrder &order, const OrderJobConfig &config,
                  int slot, bool container,
                  std::string *error) -> std::optional<sx::Workspace> {
  const std::filesystem::path slot_dir = SlotDir(config, slot);
  sx::Workspace ws;
  ws.set_source_repo(config.source_repo);
  ws.set_lower_dir((slot_dir / "repo").string());
  ws.set_base_commit(order.base_commit());
  ws.set_git(config.git);
  ws.set_staging_dir((slot_dir / "patches").string());

  for (const proto::Side *side : SidesOf(order)) {
    if (side->patch().empty()) {
      *error = "side " + side->candidate_id() + " carries no patch";
      return std::nullopt;
    }
    sx::StagedFile *file = ws.add_staged_files();
    // Named from the candidate so a slot's staging dir reads as what it is.
    file->set_path(sandbox_common::SanitizeContainerName(side->candidate_id()) +
                   ".diff");
    file->set_content(side->patch());
    ws.add_patch_files(file->path());
  }

  if (container) {
    ws.set_upper_dir((slot_dir / "overlay").string());
    ws.set_merged_dir((slot_dir / "merged").string());
    ws.set_mount_binary(config.mount);
    ws.set_umount_binary(config.umount);
    ws.set_overlay(config.host_overlay ? sx::Workspace::OVERLAY_HOST
                                       : sx::Workspace::OVERLAY_IN_SANDBOX);
    // Applied by git inside the sandbox, in both overlay modes, so the
    // workspace the build sees is the one the patch was checked against.
    ws.set_patch(sx::Workspace::PATCH_IN_ENTRYPOINT);
    ws.set_sandbox_work_dir(sandbox_common::kWorkspace);

    // Every step gets the persistent output base; only the build gets the
    // staging directory and the shared cache (see AddBuildPhase).
    sx::Mount *output_base = ws.add_mounts();
    output_base->set_source((slot_dir / "bazel_output_base").string());
    output_base->set_target(sandbox_common::kOutputBaseMount);
    return ws;
  }

  // No sandbox: the step runs in the checkout itself, and git applies the
  // patches on the host before anything builds.
  ws.set_overlay(sx::Workspace::OVERLAY_NONE);
  ws.set_upper_dir((slot_dir / "scratch").string());
  ws.set_patch(sx::Workspace::PATCH_HOST);
  return ws;
}

void AddBuildPhase(int slot, const proto::WorkOrder &order,
                   const OrderJobConfig &config, const BuildPaths &paths,
                   bool container, sx::Job *job) {
  sx::Phase *phase = job->add_phases();
  phase->set_name("build");
  // A build reaches nothing by default: a build that can fetch can also
  // exfiltrate, and a submitted genrule is arbitrary code.
  sx::Isolation *isolation = phase->mutable_isolation();
  *isolation = job->isolation();
  isolation->set_network(config.allow_build_network
                             ? sx::Isolation::NETWORK_EGRESS
                             : sx::Isolation::NETWORK_NONE);

  sx::Step *build = phase->mutable_foreground();
  build->set_name("build");
  build->set_applies_patches(true);
  if (container) {
    // The patches and the shared cache reach the build and nothing after it:
    // the thing it built has no business seeing either.
    sx::Mount *patches = build->add_mounts();
    patches->set_source(
        (config.work_dir / ("slot" + std::to_string(slot)) / "patches")
            .string());
    patches->set_target(sandbox_common::kPatchMount);
    patches->set_readonly(true);
    if (!config.disk_cache.empty()) {
      sx::Mount *disk_cache = build->add_mounts();
      disk_cache->set_source(config.disk_cache.string());
      disk_cache->set_target(sandbox_common::kDiskCacheMount);
    }
  }
  build->set_timeout_s(order.build_timeout_s() > 0 ? order.build_timeout_s()
                                                   : kDefaultTimeoutS);
  if (container) {
    *build->add_argv() = Verbatim(config.bazel);
  } else {
    *build->add_argv() = Quoted(config.bazel);
  }
  // --output_base is a startup option and belongs before the command;
  // --disk_cache and the problem's extra flags are command options and belong
  // after it. Getting that wrong is not a style question: bazel aborts with
  // "Unknown startup option", which is how the container path turned out
  // never to have built anything.
  *build->add_argv() = Verbatim("--output_base=" + paths.output_base);
  *build->add_argv() = Verbatim("build");
  if (!paths.disk_cache.empty()) {
    *build->add_argv() = Verbatim("--disk_cache=" + paths.disk_cache);
  }
  for (const std::string &flag : config.bazel_flags) {
    *build->add_argv() = Quoted(flag);
  }
  // One build, every target: both sides of a match and the referee share an
  // analysis pass and, more importantly, one consistent tree.
  for (const proto::Side *side : SidesOf(order)) {
    for (const std::string &target : side->build_targets()) {
      *build->add_argv() = Quoted(target);
    }
  }
  if (!order.referee_target().empty()) {
    *build->add_argv() = Quoted(order.referee_target());
  }
}

void AddMatchPhase(const proto::WorkOrder &order, const OrderJobConfig &config,
                   const BuildPaths &paths, bool container,
                   const sandbox_exec::Capabilities &capabilities,
                   sx::Job *job) {
  const int run_timeout_s =
      order.run_timeout_s() > 0 ? order.run_timeout_s() : kDefaultTimeoutS;
  const int match_deadline_s = order.match_deadline_s() > 0
                                   ? order.match_deadline_s()
                                   : std::max(1, run_timeout_s - 30);

  sx::Phase *phase = job->add_phases();
  phase->set_name("match");
  sx::Isolation *isolation = phase->mutable_isolation();
  *isolation = job->isolation();
  // The bots reach their referee and nothing else. There is no broker outside
  // the sandbox to dial, which is what let this stop being --network=host.
  isolation->set_network(sx::Isolation::NETWORK_PHASE_BRIDGE);
  phase->set_drain_timeout_s(match_deadline_s + 60);

  sx::Step *referee = phase->add_background();
  referee->set_name("referee");
  referee->set_keep_after_exit(true);
  *referee->add_argv() =
      Quoted(paths.bazel_bin + BinaryPathForTarget(order.referee_target()));

  std::string server;
  if (capabilities.stable_peer_names) {
    // Its own network namespace, so a fixed port cannot collide and the peer
    // resolves by name.
    referee->mutable_endpoint()->set_port(kMatchPort);
    *referee->add_argv() = Quoted("--port=" + std::to_string(kMatchPort));
    server = sandbox_exec::SandboxName(job->id(), "referee") + ":" +
             std::to_string(kMatchPort);
  } else {
    // Parallel slots share one host, so the referee binds a free port and
    // publishes the number. Waiting for that file is the difference between
    // "the bot could not connect" and "the bot connected before anything was
    // there".
    referee->mutable_endpoint()->set_discover_via_port_file(true);
    *referee->add_argv() = Quoted("--port=0");
    *referee->add_argv() = Quoted("--port_file={{port_file}}");
    server = "{{peer:referee}}";
  }
  *referee->add_argv() = Quoted("--game=" + order.game());
  *referee->add_argv() = Quoted("--games=" + std::to_string(order.num_games()));
  *referee->add_argv() =
      Quoted("--player_a=" + order.candidate().candidate_id());
  *referee->add_argv() = Quoted("--player_b=" + order.opponent_spec());
  if (!container) {
    *referee->add_argv() = Quoted("--scratch_dir={{scratch}}");
  }
  *referee->add_argv() =
      Quoted("--deadline_s=" + std::to_string(match_deadline_s));
  // Opaque to the worker: whatever the problem set, handed to the registry
  // linked into the referee. Omitted entirely when empty so an order that
  // sets nothing produces the argv it always did.
  if (!order.registry_options().empty()) {
    *referee->add_argv() =
        Quoted("--registry_options=" +
               kv_options::Format({order.registry_options().begin(),
                                   order.registry_options().end()}));
  }

  const auto add_bot = [&](sx::Step *step, const proto::Side &side,
                           const std::string &opponent) {
    step->set_keep_after_exit(true);
    *step->add_argv() =
        Quoted(paths.bazel_bin + BinaryPathForTarget(side.bot_target()));
    for (const std::string &arg :
         BotArgs(side.candidate_id(), server, opponent, order.num_games(),
                 FormatParams(side.params()))) {
      *step->add_argv() = Quoted(arg);
    }
  };

  if (order.has_opponent()) {
    // The opponent plays for the whole match while the primary bot runs in
    // the foreground. Both stop when the referee closes their streams.
    sx::Step *opponent = phase->add_background();
    opponent->set_name("opponent");
    add_bot(opponent, order.opponent(),
            std::string(kPlayerPrefix) + order.candidate().candidate_id());
  }

  sx::Step *bot = phase->mutable_foreground();
  bot->set_name("bot");
  bot->set_timeout_s(run_timeout_s);
  *bot->mutable_isolation() = SolutionIsolation(config, container, *isolation);
  add_bot(bot, order.candidate(), order.opponent_spec());
}

void AddGradePhases(const proto::WorkOrder &order, const OrderJobConfig &config,
                    bool container, sx::Job *job) {
  const proto::GradeOrder &grade = order.grade();
  const int repeats = std::max(1, grade.repeats());
  const int timeout_s =
      grade.timeout_s() > 0 ? grade.timeout_s() : kDefaultTimeoutS;

  // One phase per run rather than one phase with N steps: each run is
  // independent, and a run that hangs should not be waited on by the next.
  for (int run = 0; run < repeats; ++run) {
    sx::Phase *phase = job->add_phases();
    phase->set_name("grade");
    sx::Isolation *isolation = phase->mutable_isolation();
    *isolation = job->isolation();
    // A solution timed against a stopwatch has no business reaching the
    // network.
    isolation->set_network(sx::Isolation::NETWORK_NONE);

    sx::Step *step = phase->mutable_foreground();
    step->set_name("grade");
    step->set_timeout_s(timeout_s);
    *step->mutable_isolation() =
        SolutionIsolation(config, container, *isolation);
    // Exported rather than fixed, so the command needs no knowledge of the
    // sandbox's directory layout.
    // The engine resolves {{scratch}} to wherever the step can write: a
    // mount point inside a container, a host path without one.
    (*step->mutable_env())["ARENA_REPORT"] = "{{scratch}}/report.json";
    step->add_collect_files("report.json");
    for (const std::string &word : grade.argv()) {
      *step->add_argv() = Quoted(word);
    }
    if (!container) {
      // The process engine runs argv[0] directly, so it must resolve in the
      // checkout rather than through a shell's PATH.
      step->set_cwd("");
    }
  }
}

}  // namespace

auto SlotLogDir(const OrderJobConfig &config,
                int slot) -> std::filesystem::path {
  return SlotDir(config, slot) / "logs";
}

auto JobForOrder(int slot, const proto::WorkOrder &order,
                 const OrderJobConfig &config,
                 const sandbox_exec::Capabilities &capabilities, sx::Job *job,
                 std::string *error) -> bool {
  const bool container = capabilities.isolates;

  job->set_id(sandbox_exec::SandboxName(
      "saw-" + std::to_string(slot) + "-" + order.order_id(), ""));
  job->set_log_dir(SlotLogDir(config, slot).string());
  *job->mutable_isolation() = Isolation(config, container);

  std::optional<sx::Workspace> workspace =
      WorkspaceFor(order, config, slot, container, error);
  if (!workspace.has_value()) {
    return false;
  }
  *job->mutable_workspace() = *workspace;

  const BuildPaths paths = PathsFor(config, slot, container);
  AddBuildPhase(slot, order, config, paths, container, job);

  if (order.has_grade()) {
    AddGradePhases(order, config, container, job);
  } else {
    AddMatchPhase(order, config, paths, container, capabilities, job);
  }
  return true;
}

}  // namespace tournament_arena
