#include "game_arena/sandbox/worker/order_job.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "game_arena/common/kv_options/kv_options.h"
#include "game_arena/sandbox/common/docker.h"

namespace tournament_arena {

namespace sx = sandbox_exec::proto;

namespace {

constexpr int kDefaultTimeoutS = 1800;
// Fixed: a match's network namespace is private, so nothing can collide.
constexpr int kMatchPort = 50051;

sx::Token Verbatim(const std::string& text) {
  sx::Token token;
  token.set_text(text);
  token.set_verbatim(true);
  return token;
}

sx::Token Quoted(const std::string& text) {
  sx::Token token;
  token.set_text(text);
  return token;
}

// "//a/b:c" -> "a/b/c" and "//a/b" -> "a/b/b": its binary under bazel-bin.
std::string BinaryPathForTarget(const std::string& target) {
  std::string label(target);
  if (label.rfind("//", 0) == 0) {
    label = label.substr(2);
  }
  const auto colon = label.rfind(':');
  if (colon != std::string::npos) {
    return label.substr(0, colon) + "/" + label.substr(colon + 1);
  }
  const auto slash = label.rfind('/');
  return slash == std::string::npos ? label + "/" + label
                                    : label + "/" + label.substr(slash + 1);
}

std::filesystem::path SlotDir(const OrderJobConfig& config, int slot) {
  return config.work_dir / ("slot" + std::to_string(slot));
}

sx::Isolation Isolation(const proto::SandboxOrder& sandbox, bool container) {
  sx::Isolation isolation;
  if (!container) {
    // No cgroups; RLIMIT_AS goes on the solution's steps only, since bazel's
    // JVM dies at startup under any cap a problem means for a solution.
    return isolation;
  }
  isolation.set_image(sandbox.image());
  isolation.set_memory_limit_mb(sandbox.memory_limit_mb());
  isolation.set_cpus(sandbox.cpus());
  isolation.set_pids_limit(sandbox.pids_limit());
  isolation.set_run_as_user(sandbox.run_as_user());
  sx::Tmpfs* tmpfs = isolation.add_tmpfs();
  tmpfs->set_target("/tmp");
  tmpfs->set_options("exec");
  return isolation;
}

// For the steps that run the submission: the memory limit as RLIMIT_AS.
sx::Isolation SolutionIsolation(const proto::SandboxOrder& sandbox,
                                bool container, const sx::Isolation& base) {
  sx::Isolation isolation = base;
  if (!container && sandbox.memory_limit_mb() > 0) {
    isolation.set_address_space_limit_bytes(
        static_cast<std::uint64_t>(sandbox.memory_limit_mb()) * 1024 * 1024);
  }
  return isolation;
}

// As a container sees them, or host paths for the process engine.
struct BuildPaths {
  std::string output_base;
  std::string disk_cache;
  std::string bazel_bin;
};

BuildPaths PathsFor(const OrderJobConfig& config, int slot, bool container) {
  BuildPaths paths;
  if (container) {
    paths.output_base = sandbox_common::kOutputBaseMount;
    paths.disk_cache = sandbox_common::kDiskCacheMount;
    paths.bazel_bin = "./bazel-bin/";
    return paths;
  }
  paths.output_base = (SlotDir(config, slot) / "bazel_output_base").string();
  paths.disk_cache = config.disk_cache.string();
  paths.bazel_bin =
      (SlotDir(config, slot) / "repo" / "bazel-bin").string() + "/";
  return paths;
}

sx::Mount PersistentMount(const std::filesystem::path& bind_dir,
                          const std::string& volume,
                          const std::string& target) {
  sx::Mount mount;
  if (bind_dir.empty()) {
    mount.set_kind(sx::Mount::VOLUME);
    mount.set_source(volume);
  } else {
    mount.set_kind(sx::Mount::BIND);
    mount.set_source(bind_dir.string());
  }
  mount.set_target(target);
  return mount;
}

// A cache is its writer's: what root left in one, another user cannot replace.
std::string OwnerSuffix(const proto::WorkOrder& order) {
  const std::string& user = order.sandbox().run_as_user();
  return user.empty() ? "" : "-u" + sandbox_common::SanitizeContainerName(user);
}

// Read-only after the build: it outlives the order, and a bot that could
// write it could change what the slot's next order is built from.
std::optional<sx::Mount> OutputBaseMount(const proto::WorkOrder& order,
                                         const OrderJobConfig& config, int slot,
                                         bool container) {
  if (!container) {
    return std::nullopt;
  }
  const std::string slot_name = "slot" + std::to_string(slot);
  return PersistentMount(config.bind_output_base_dir.empty()
                             ? std::filesystem::path()
                             : config.bind_output_base_dir / slot_name,
                         config.volume_prefix + "-" + slot_name +
                             "-output_base" + OwnerSuffix(order),
                         sandbox_common::kOutputBaseMount);
}

void MountOutputBase(const std::optional<sx::Mount>& output_base, bool readonly,
                     sx::Step* step) {
  if (output_base.has_value()) {
    sx::Mount* mount = step->add_mounts();
    *mount = *output_base;
    mount->set_readonly(readonly);
  }
}

// A builtin plays inside the referee: nothing of it to patch, build or start.
bool IsBuiltin(const proto::Side& side) {
  return side.candidate_id().starts_with("builtin:");
}

std::vector<const proto::Side*> SidesOf(const proto::WorkOrder& order) {
  std::vector<const proto::Side*> sides;
  if (!IsBuiltin(order.candidate())) {
    sides.push_back(&order.candidate());
  }
  if (order.has_opponent()) {
    sides.push_back(&order.opponent());
  }
  return sides;
}

std::optional<sx::Workspace> WorkspaceFor(const proto::WorkOrder& order,
                                          const OrderJobConfig& config,
                                          int slot, bool container,
                                          std::string* error) {
  const std::filesystem::path slot_dir = SlotDir(config, slot);
  sx::Workspace ws;
  ws.set_git(config.git);
  ws.set_staging_dir((slot_dir / "patches").string());

  for (const proto::Side* side : SidesOf(order)) {
    if (side->patch().empty()) {
      *error = "side " + side->candidate_id() + " carries no patch";
      return std::nullopt;
    }
    sx::StagedFile* file = ws.add_staged_files();
    file->set_path(sandbox_common::SanitizeContainerName(side->candidate_id()) +
                   ".diff");
    file->set_content(side->patch());
    ws.add_patch_files(file->path());
  }

  if (container) {
    // The tree is the image's; git applies the patches inside the sandbox.
    ws.set_patch(sx::Workspace::PATCH_IN_ENTRYPOINT);
    ws.set_sandbox_work_dir(sandbox_common::kWorkspace);
    return ws;
  }

  // The slot's own tree, which its caller fills, patched on the host.
  ws.set_tree_dir((slot_dir / "repo").string());
  ws.set_scratch_dir((slot_dir / "scratch").string());
  ws.set_patch(sx::Workspace::PATCH_HOST);
  return ws;
}

void AddBuildPhase(const proto::WorkOrder& order, const OrderJobConfig& config,
                   const BuildPaths& paths,
                   const std::optional<sx::Mount>& output_base, bool container,
                   sx::Job* job) {
  sx::Phase* phase = job->add_phases();
  phase->set_name("build");
  // A build that can fetch can exfiltrate: a submitted genrule is any code.
  sx::Isolation* isolation = phase->mutable_isolation();
  *isolation = job->isolation();
  isolation->set_network(order.sandbox().allow_build_network()
                             ? sx::Isolation::NETWORK_EGRESS
                             : sx::Isolation::NETWORK_NONE);
  // The solution's memory and pid caps are not the build's: bazel's JVM and a
  // few dozen compilers exceed any limit a problem means for a bot.
  isolation->set_memory_limit_mb(order.sandbox().build_memory_limit_mb());
  isolation->set_pids_limit(0);

  sx::Step* build = phase->mutable_foreground();
  build->set_name("build");
  build->set_applies_patches(true);
  MountOutputBase(output_base, /*readonly=*/false, build);
  if (container) {
    // The shared cache reaches the build and nothing it built.
    *build->add_mounts() = PersistentMount(
        config.bind_disk_cache_dir,
        config.volume_prefix + "-disk_cache" + OwnerSuffix(order),
        sandbox_common::kDiskCacheMount);
  }
  build->set_timeout_s(order.build_timeout_s() > 0 ? order.build_timeout_s()
                                                   : kDefaultTimeoutS);
  *build->add_argv() = Verbatim(config.bazel);
  // Startup options before `build`, the rest after, or bazel aborts with
  // "Unknown startup option".
  *build->add_argv() = Verbatim("--output_base=" + paths.output_base);
  if (container) {
    // Bazel unpacks itself into the slot's volume, as the image is read-only.
    // A user root, not --install_base: a fixed one breaks on the next bazel.
    *build->add_argv() =
        Verbatim("--output_user_root=" + paths.output_base + "/_user_root");
  }
  *build->add_argv() = Verbatim("build");
  if (!paths.disk_cache.empty()) {
    *build->add_argv() = Verbatim("--disk_cache=" + paths.disk_cache);
  }
  for (const std::string& flag : order.bazel_flags()) {
    *build->add_argv() = Quoted(flag);
  }
  // One build for every target, so both sides and the referee share a tree.
  for (const proto::Side* side : SidesOf(order)) {
    for (const std::string& target : side->build_targets()) {
      *build->add_argv() = Quoted(target);
    }
  }
  if (!order.referee_target().empty()) {
    *build->add_argv() = Quoted(order.referee_target());
  }
}

void AddMatchPhase(const proto::WorkOrder& order, const BuildPaths& paths,
                   const std::optional<sx::Mount>& output_base, bool container,
                   const sandbox_exec::Capabilities& capabilities,
                   sx::Job* job) {
  const int run_timeout_s =
      order.run_timeout_s() > 0 ? order.run_timeout_s() : kDefaultTimeoutS;
  const int match_deadline_s = order.match_deadline_s() > 0
                                   ? order.match_deadline_s()
                                   : std::max(1, run_timeout_s - 30);

  sx::Phase* phase = job->add_phases();
  phase->set_name("match");
  sx::Isolation* isolation = phase->mutable_isolation();
  *isolation = job->isolation();
  // The bots reach their referee and nothing else.
  isolation->set_network(sx::Isolation::NETWORK_PHASE_BRIDGE);
  phase->set_drain_timeout_s(match_deadline_s + 60);

  // Two builtins: the referee plays the whole match, so its exit ends it.
  const bool bot_less = IsBuiltin(order.candidate());
  sx::Step* referee =
      bot_less ? phase->mutable_foreground() : phase->add_background();
  referee->set_name("referee");
  referee->set_keep_after_exit(true);
  MountOutputBase(output_base, /*readonly=*/true, referee);
  // The match's result: in a scratch of its own, where neither bot can reach.
  referee->set_private_scratch(true);
  referee->add_collect_files(kMatchReport);
  *referee->add_argv() =
      Quoted(paths.bazel_bin + BinaryPathForTarget(order.referee_target()));

  std::string server;
  if (capabilities.stable_peer_names) {
    *referee->add_argv() = Quoted("--port=" + std::to_string(kMatchPort));
    server = sandbox_exec::SandboxName(job->id(), "referee") + ":" +
             std::to_string(kMatchPort);
  } else {
    // Parallel slots share the host: a free port, published in a file.
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
  *referee->add_argv() = Quoted("--scratch_dir={{scratch}}");
  *referee->add_argv() =
      Quoted("--report={{scratch}}/" + std::string(kMatchReport));
  *referee->add_argv() =
      Quoted("--deadline_s=" + std::to_string(match_deadline_s));
  // Omitted when unset, so the referee keeps its own defaults.
  if (order.turn_timeout_ms() > 0) {
    *referee->add_argv() =
        Quoted("--turn_timeout_ms=" + std::to_string(order.turn_timeout_ms()));
  }
  if (order.game_time_budget_ms() > 0) {
    *referee->add_argv() = Quoted("--game_time_budget_ms=" +
                                  std::to_string(order.game_time_budget_ms()));
  }
  if (order.max_moves_per_game() > 0) {
    *referee->add_argv() = Quoted("--max_moves_per_game=" +
                                  std::to_string(order.max_moves_per_game()));
  }
  // Opaque to the worker: for the registry linked into the referee.
  if (!order.registry_options().empty()) {
    *referee->add_argv() =
        Quoted("--registry_options=" +
               kv_options::Format({order.registry_options().begin(),
                                   order.registry_options().end()}));
  }

  const auto add_bot = [&](sx::Step* step, const proto::Side& side,
                           const std::string& opponent) {
    step->set_keep_after_exit(true);
    MountOutputBase(output_base, /*readonly=*/true, step);
    *step->add_argv() =
        Quoted(paths.bazel_bin + BinaryPathForTarget(side.bot_target()));
    *step->add_argv() = Quoted("--name=" + side.candidate_id());
    *step->add_argv() = Quoted("--server=" + server);
    *step->add_argv() = Quoted("--opponent=" + opponent);
    *step->add_argv() = Quoted("--games=" + std::to_string(order.num_games()));
    // Sorted, so a rebuilt candidate gets a byte-identical command line.
    std::string params;
    for (const auto& [key, value] : std::map<std::string, std::string>(
             side.params().begin(), side.params().end())) {
      params += (params.empty() ? "" : ",") + key + "=" + value;
    }
    if (!params.empty()) {
      *step->add_argv() = Quoted("--params=" + params);
    }
  };

  if (bot_less) {
    referee->set_timeout_s(run_timeout_s);
    return;
  }
  if (order.has_opponent()) {
    // Plays the whole match; the referee ends both bots' streams.
    sx::Step* opponent = phase->add_background();
    opponent->set_name("opponent");
    add_bot(opponent, order.opponent(),
            "player:" + order.candidate().candidate_id());
  }

  sx::Step* bot = phase->mutable_foreground();
  bot->set_name("bot");
  bot->set_timeout_s(run_timeout_s);
  *bot->mutable_isolation() =
      SolutionIsolation(order.sandbox(), container, *isolation);
  add_bot(bot, order.candidate(), order.opponent_spec());
}

void AddGradePhases(const proto::WorkOrder& order,
                    const std::optional<sx::Mount>& output_base, bool container,
                    sx::Job* job) {
  const proto::GradeOrder& grade = order.grade();
  const int repeats = std::max(1, grade.repeats());
  const int timeout_s =
      grade.timeout_s() > 0 ? grade.timeout_s() : kDefaultTimeoutS;

  // A phase per run, so a run that hangs does not hold up the next.
  for (int run = 0; run < repeats; ++run) {
    sx::Phase* phase = job->add_phases();
    phase->set_name("grade");
    sx::Isolation* isolation = phase->mutable_isolation();
    *isolation = job->isolation();
    // A solution timed against a stopwatch has no business on the network.
    isolation->set_network(sx::Isolation::NETWORK_NONE);

    sx::Step* step = phase->mutable_foreground();
    step->set_name("grade");
    step->set_timeout_s(timeout_s);
    MountOutputBase(output_base, /*readonly=*/true, step);
    *step->mutable_isolation() =
        SolutionIsolation(order.sandbox(), container, *isolation);
    // The engine resolves {{scratch}}: the command knows no layout.
    (*step->mutable_env())["ARENA_REPORT"] = "{{scratch}}/report.json";
    step->add_collect_files("report.json");
    for (const std::string& word : grade.argv()) {
      *step->add_argv() = Quoted(word);
    }
  }
}

}  // namespace

std::map<std::string, std::string> BuildKeys(const proto::WorkOrder& order) {
  // A submission is sources only (the arena writes its BUILD), so no build
  // runs its code or can touch another target's outputs.
  std::string base = order.sandbox().image();
  for (const std::string& flag : order.bazel_flags()) {
    base += " " + flag;
  }
  std::map<std::string, std::string> keys;
  for (const proto::Side* side : SidesOf(order)) {
    for (const std::string& target : side->build_targets()) {
      keys[target] =
          base + " " + std::to_string(std::hash<std::string>{}(side->patch()));
    }
  }
  if (!order.referee_target().empty()) {
    keys[order.referee_target()] = base;
  }
  return keys;
}

std::filesystem::path SlotLogDir(const OrderJobConfig& config, int slot) {
  return SlotDir(config, slot) / "logs";
}

bool JobForOrder(int slot, const proto::WorkOrder& order,
                 const OrderJobConfig& config,
                 const sandbox_exec::Capabilities& capabilities, sx::Job* job,
                 std::string* error, bool build) {
  const bool container = capabilities.isolates;

  job->set_id(sandbox_exec::SandboxName(
      "saw-" + std::to_string(slot) + "-" + order.order_id(), ""));
  job->set_log_dir(SlotLogDir(config, slot).string());
  *job->mutable_isolation() = Isolation(order.sandbox(), container);

  std::optional<sx::Workspace> workspace =
      WorkspaceFor(order, config, slot, container, error);
  if (!workspace.has_value()) {
    return false;
  }
  *job->mutable_workspace() = *workspace;

  const BuildPaths paths = PathsFor(config, slot, container);
  const std::optional<sx::Mount> output_base =
      OutputBaseMount(order, config, slot, container);
  if (build) {
    AddBuildPhase(order, config, paths, output_base, container, job);
  }

  if (order.has_grade()) {
    AddGradePhases(order, output_base, container, job);
  } else {
    AddMatchPhase(order, paths, output_base, container, capabilities, job);
  }
  return true;
}

}  // namespace tournament_arena
