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

sx::Token Verbatim(const std::string &text) {
  sx::Token token;
  token.set_text(text);
  token.set_verbatim(true);
  return token;
}

sx::Token Quoted(const std::string &text) {
  sx::Token token;
  token.set_text(text);
  return token;
}

std::filesystem::path SlotDir(const OrderJobConfig &config, int slot) {
  return config.work_dir / ("slot" + std::to_string(slot));
}

sx::Isolation Isolation(const proto::SandboxOrder &sandbox, bool container) {
  sx::Isolation isolation;
  if (!container) {
    // No image and no cgroups. The one limit this engine can apply is an
    // address-space cap, and it goes on the steps that run submitted code
    // rather than here -- see SolutionIsolation. A cap on the build would be
    // a cap on bazel, whose JVM reserves far more address space than any
    // limit a problem means for a solution, and it dies at startup.
    return isolation;
  }
  isolation.set_image(sandbox.image());
  isolation.set_memory_limit_mb(sandbox.memory_limit_mb());
  isolation.set_cpus(sandbox.cpus());
  isolation.set_pids_limit(sandbox.pids_limit());
  isolation.set_run_as_user(sandbox.run_as_user());
  // Nothing to mount and nothing to be privileged for: the defaults (drop
  // every capability, no new privileges, a read-only root) stand, and /tmp
  // is the one writable place besides the tree and the scratch volume.
  sx::Tmpfs *tmpfs = isolation.add_tmpfs();
  tmpfs->set_target("/tmp");
  tmpfs->set_options("exec");
  return isolation;
}

// The isolation for a step that runs the submission itself: the problem's
// memory limit, as an address-space cap the process engine can enforce. Not
// applied to the build, for the reason in Isolation() above.
sx::Isolation SolutionIsolation(const proto::SandboxOrder &sandbox,
                                bool container, const sx::Isolation &base) {
  sx::Isolation isolation = base;
  if (!container && sandbox.memory_limit_mb() > 0) {
    isolation.set_address_space_limit_bytes(
        static_cast<std::uint64_t>(sandbox.memory_limit_mb()) * 1024 * 1024);
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

BuildPaths PathsFor(const OrderJobConfig &config, int slot, bool container) {
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

// A persistent directory for a container: a docker volume unless this host
// opted into a bind mount for it.
sx::Mount PersistentMount(const std::filesystem::path &bind_dir,
                          const std::string &volume,
                          const std::string &target) {
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

std::vector<const proto::Side *> SidesOf(const proto::WorkOrder &order) {
  std::vector<const proto::Side *> sides = {&order.candidate()};
  if (order.has_opponent()) {
    sides.push_back(&order.opponent());
  }
  return sides;
}

std::optional<sx::Workspace> WorkspaceFor(const proto::WorkOrder &order,
                                          const OrderJobConfig &config,
                                          int slot, bool container,
                                          std::string *error) {
  const std::filesystem::path slot_dir = SlotDir(config, slot);
  sx::Workspace ws;
  ws.set_source_repo(order.repo_url());
  ws.set_tree_dir((slot_dir / "repo").string());
  ws.set_base_commit(order.base_commit());
  ws.set_git(config.git);
  ws.set_tar(config.tar);
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
    // The engine copies the tree in; git applies the patches inside the
    // sandbox, so the workspace the build sees is the one the patch was
    // checked against.
    ws.set_patch(sx::Workspace::PATCH_IN_ENTRYPOINT);
    ws.set_sandbox_work_dir(sandbox_common::kWorkspace);

    // Every step gets the persistent output base; only the build gets the
    // staged files and the shared cache (see AddBuildPhase).
    const std::string slot_name = "slot" + std::to_string(slot);
    *ws.add_mounts() =
        PersistentMount(config.bind_output_base_dir.empty()
                            ? std::filesystem::path()
                            : config.bind_output_base_dir / slot_name,
                        config.volume_prefix + "-" + slot_name + "-output_base",
                        sandbox_common::kOutputBaseMount);
    return ws;
  }

  // No sandbox: the step runs in the checkout itself, and git applies the
  // patches on the host before anything builds.
  ws.set_scratch_dir((slot_dir / "scratch").string());
  ws.set_patch(sx::Workspace::PATCH_HOST);
  return ws;
}

void AddBuildPhase(const proto::WorkOrder &order, const OrderJobConfig &config,
                   const BuildPaths &paths, bool container, sx::Job *job) {
  sx::Phase *phase = job->add_phases();
  phase->set_name("build");
  // A build reaches nothing by default: a build that can fetch can also
  // exfiltrate, and a submitted genrule is arbitrary code.
  sx::Isolation *isolation = phase->mutable_isolation();
  *isolation = job->isolation();
  isolation->set_network(order.sandbox().allow_build_network()
                             ? sx::Isolation::NETWORK_EGRESS
                             : sx::Isolation::NETWORK_NONE);
  // The problem's memory and pid caps are for the solution's run, as the
  // process engine already treats them (SolutionIsolation): a build is the
  // problem's own toolchain, and bazel's JVM plus a few dozen compilers is
  // more than any limit a problem means for a bot. The build keeps every
  // other part of the sandbox and its own timeout.
  isolation->set_memory_limit_mb(0);
  isolation->set_pids_limit(0);

  sx::Step *build = phase->mutable_foreground();
  build->set_name("build");
  build->set_applies_patches(true);
  if (container) {
    // The shared cache reaches the build and nothing after it: the thing it
    // built has no business seeing it. (The staged patches reach the build
    // the same way, arranged by the engine for the step that applies them.)
    *build->add_mounts() = PersistentMount(config.bind_disk_cache_dir,
                                           config.volume_prefix + "-disk_cache",
                                           sandbox_common::kDiskCacheMount);
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
  if (container) {
    // Bazel's own installation, unpacked once per slot into the persistent
    // volume beside the output base rather than into the image: the install
    // base wants a lock file next to itself, and the image is read-only.
    // Given as an output_user_root, not an --install_base: under one, bazel
    // names the install directory after the hash of its own binary. The
    // volume outlives the image, and a fixed install base unpacked by one
    // bazel is "corrupt installation" to the next -- every build failing, on
    // the first sandbox image that moves to another release, until someone
    // works out which volume to delete.
    *build->add_argv() =
        Verbatim("--output_user_root=" + paths.output_base + "/_user_root");
  }
  *build->add_argv() = Verbatim("build");
  if (!paths.disk_cache.empty()) {
    *build->add_argv() = Verbatim("--disk_cache=" + paths.disk_cache);
  }
  for (const std::string &flag : order.bazel_flags()) {
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

void AddMatchPhase(const proto::WorkOrder &order, const BuildPaths &paths,
                   bool container,
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
  // Omitted when the problem said nothing, so the referee keeps its own
  // default rather than being handed a zero that means something else.
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
  *bot->mutable_isolation() =
      SolutionIsolation(order.sandbox(), container, *isolation);
  add_bot(bot, order.candidate(), order.opponent_spec());
}

void AddGradePhases(const proto::WorkOrder &order, bool container,
                    sx::Job *job) {
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
        SolutionIsolation(order.sandbox(), container, *isolation);
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

std::filesystem::path SlotLogDir(const OrderJobConfig &config, int slot) {
  return SlotDir(config, slot) / "logs";
}

bool JobForOrder(int slot, const proto::WorkOrder &order,
                 const OrderJobConfig &config,
                 const sandbox_exec::Capabilities &capabilities, sx::Job *job,
                 std::string *error) {
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
  AddBuildPhase(order, config, paths, container, job);

  if (order.has_grade()) {
    AddGradePhases(order, container, job);
  } else {
    AddMatchPhase(order, paths, container, capabilities, job);
  }
  return true;
}

}  // namespace tournament_arena
