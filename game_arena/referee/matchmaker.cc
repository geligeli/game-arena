#include "game_arena/referee/matchmaker.h"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/log/log.h"
#include "absl/strings/str_join.h"
#include "game_arena/referee/game_registry.h"

namespace tournament_broker {

namespace {

constexpr std::string_view kBuiltinPrefix = "builtin:";
constexpr std::string_view kPlayerPrefix = "player:";

// Order-independent, so every member of a group computes the same key.
std::string GroupKey(const std::string& game,
                     std::vector<std::string> members) {
  std::ranges::sort(members);
  return game + "\t" + absl::StrJoin(members, "\t");
}

// |opponent| is "builtin:<spec>"; nullopt, with *error set, for a bad spec.
std::optional<Seat> BuiltinSeat(const GameDescriptor& descriptor,
                                const std::string& opponent,
                                std::string* error) {
  auto builtin = descriptor.make_builtin(
      std::string_view(opponent).substr(kBuiltinPrefix.size()), error);
  if (!builtin.has_value()) {
    return std::nullopt;
  }
  return Seat{.display_name = opponent,
              .client = nullptr,
              .builtin = std::move(*builtin)};
}

}  // namespace

Matchmaker::Matchmaker(MatchmakerConfig config, GameHistory* history)
    : config_(config),
      history_(history),
      pool_(config.worker_threads > 0
                ? config.worker_threads
                : static_cast<int>(std::thread::hardware_concurrency())) {
  reaper_ = std::thread(&Matchmaker::ReaperLoop, this);
}

Matchmaker::~Matchmaker() {
  Shutdown();
  reaper_.join();
  // Timer first, or a pending deadline posts onto a draining pool.
  timer_.Stop();
  pool_.Stop();
}

bool Matchmaker::Join(std::shared_ptr<ClientHandle> client,
                      const proto::Hello& hello, std::string* error) {
  {
    std::lock_guard lock(mutex_);
    if (stopping_) {
      // Shutdown() has swept already; nothing would clean up a later join.
      *error = "server is shutting down";
      return false;
    }
  }

  const auto& registry = GameRegistry();
  const auto it = registry.find(hello.game());
  if (it == registry.end()) {
    *error = "unknown game '" + hello.game() + "'";
    return false;
  }
  const GameDescriptor& descriptor = it->second;
  if (hello.opponent_size() + 1 != descriptor.num_players) {
    *error = "'" + hello.game() + "' seats " +
             std::to_string(descriptor.num_players) + ", so it takes " +
             std::to_string(descriptor.num_players - 1) + " opponent(s), not " +
             std::to_string(hello.opponent_size());
    return false;
  }

  std::vector<Seat> builtins;
  std::vector<std::string> members = {client->name()};
  for (const std::string& opponent : hello.opponent()) {
    if (opponent.starts_with(kBuiltinPrefix)) {
      std::optional<Seat> bot = BuiltinSeat(descriptor, opponent, error);
      if (!bot.has_value()) {
        return false;
      }
      builtins.push_back(std::move(*bot));
      continue;
    }
    if (!opponent.starts_with(kPlayerPrefix)) {
      *error = "unknown opponent '" + opponent +
               "' (expected: builtin:<spec> | player:<name>)";
      return false;
    }
    const std::string wanted = opponent.substr(kPlayerPrefix.size());
    if (wanted.empty()) {
      *error = "empty partner name in opponent 'player:'";
      return false;
    }
    if (std::ranges::contains(members, wanted)) {
      *error = "'" + wanted + "' cannot take two seats";
      return false;
    }
    members.push_back(wanted);
  }

  if (members.size() == 1) {
    builtins.push_back(Seat{.display_name = client->name(),
                            .client = std::move(client),
                            .builtin = nullptr});
    StartGroupGame(descriptor, std::move(builtins));
    return true;
  }
  return JoinRendezvous(std::move(client), descriptor, std::move(members),
                        std::move(builtins), error);
}

bool Matchmaker::StartBuiltins(const std::string& game,
                               const std::vector<std::string>& specs,
                               std::string* error) {
  const GameDescriptor& descriptor = GameRegistry().at(game);
  std::vector<Seat> seats;
  for (const std::string& spec : specs) {
    std::optional<Seat> seat = BuiltinSeat(descriptor, spec, error);
    if (!seat.has_value()) {
      return false;
    }
    seats.push_back(std::move(*seat));
  }
  StartGroupGame(descriptor, std::move(seats));
  return true;
}

bool Matchmaker::JoinRendezvous(std::shared_ptr<ClientHandle> client,
                                const GameDescriptor& descriptor,
                                std::vector<std::string> members,
                                std::vector<Seat> builtins,
                                std::string* error) {
  std::vector<std::string> everyone = members;
  for (const Seat& builtin : builtins) {
    everyone.push_back(builtin.display_name);
  }
  const std::string key = GroupKey(descriptor.name, std::move(everyone));
  std::vector<Parked> arrived;
  {
    std::lock_guard lock(mutex_);
    if (stopping_) {
      *error = "server is shutting down";
      return false;
    }
    std::vector<Parked>& waiting = rendezvous_[key];
    std::erase_if(waiting,
                  [](const Parked& p) { return p.client->disconnected(); });
    // Same key and same name: a duplicate connection, not a member.
    if (std::ranges::any_of(waiting, [&](const Parked& p) {
          return p.client->name() == client->name();
        })) {
      *error = "another connection is already waiting as '" + client->name() +
               "' for '" + absl::StrJoin(members, "', '") + "'";
      return false;
    }
    waiting.push_back(Parked{.client = std::move(client),
                             .game = descriptor.name,
                             .deadline = std::chrono::steady_clock::now() +
                                         config_.rendezvous_timeout});
    if (waiting.size() < members.size()) {
      reaper_cv_.notify_all();
      return true;
    }
    arrived = std::move(waiting);
    rendezvous_.erase(key);
  }

  for (Parked& parked : arrived) {
    builtins.push_back(Seat{.display_name = parked.client->name(),
                            .client = std::move(parked.client),
                            .builtin = nullptr});
  }
  StartGroupGame(descriptor, std::move(builtins));
  return true;
}

void Matchmaker::StartGroupGame(const GameDescriptor& descriptor,
                                std::vector<Seat> seats) {
  const auto by_name = [](const Seat& a, const Seat& b) {
    return a.display_name < b.display_name;
  };
  std::ranges::sort(seats, by_name);
  std::vector<std::string> names;
  for (const Seat& seat : seats) {
    names.push_back(seat.display_name);
  }
  const std::string key = GroupKey(descriptor.name, names);
  uint64_t played = 0;
  {
    std::lock_guard lock(mutex_);
    played = group_games_[key]++;
  }
  // By name, then by the group's game count: neither who arrived first nor
  // which members are builtins decides who moves first. Every n games are
  // the n rotations of one order of the rest behind the first; the next n
  // take the next order.
  const std::size_t n = seats.size();
  std::size_t orders = 1;
  for (std::size_t i = 2; i <= n; ++i) {
    orders *= i;
  }
  const std::size_t p = played % orders;
  for (std::size_t i = 0; i < p / n; ++i) {
    std::next_permutation(seats.begin() + 1, seats.end(), by_name);
  }
  std::ranges::rotate(seats, seats.begin() + static_cast<long>(p % n));
  StartGame(descriptor, std::move(seats));
}

void Matchmaker::StartGame(const GameDescriptor& descriptor,
                           std::vector<Seat> seats) {
  GameRunConfig run_config;
  run_config.turn_timeout = config_.turn_timeout;
  run_config.on_record = config_.on_record;
  run_config.game_time_budget = config_.game_time_budget;
  run_config.max_moves_per_game = config_.max_moves_per_game;

  const uint64_t id = ++game_counter_;
  ++running_games_;
  auto run =
      std::make_shared<GameRun>(descriptor, run_config, std::move(seats), id,
                                history_, &pool_, &timer_, [this, id] {
                                  {
                                    std::lock_guard lock(mutex_);
                                    running_.erase(id);
                                  }
                                  if (--running_games_ == 0) {
                                    std::lock_guard lock(drain_mutex_);
                                    drain_cv_.notify_all();
                                  }
                                });
  {
    std::lock_guard lock(mutex_);
    running_.emplace(id, run);
  }
  run->Start();
}

void Matchmaker::Shutdown() {
  std::vector<std::shared_ptr<ClientHandle>> waiting;
  std::vector<std::shared_ptr<GameRun>> games;
  {
    std::lock_guard lock(mutex_);
    if (stopping_) {
      return;
    }
    stopping_ = true;
    for (auto& [key, group] : rendezvous_) {
      for (Parked& parked : group) {
        waiting.push_back(parked.client);
      }
    }
    rendezvous_.clear();
    for (auto& [id, weak] : running_) {
      if (auto run = weak.lock()) {
        games.push_back(std::move(run));
      }
    }
  }
  reaper_cv_.notify_all();

  for (const std::shared_ptr<ClientHandle>& client : waiting) {
    client->MarkDisconnected();
    client->CloseAfterFlush();
  }
  for (const std::shared_ptr<GameRun>& run : games) {
    run->Abort("server_shutdown");
  }
}

void Matchmaker::ReaperLoop() {
  std::unique_lock lock(mutex_);
  while (!stopping_) {
    // A refused join can leave a group with no one in it: nothing to wait on.
    std::erase_if(rendezvous_,
                  [](const auto& entry) { return entry.second.empty(); });
    if (rendezvous_.empty()) {
      reaper_cv_.wait(lock);
      continue;
    }
    auto earliest = std::chrono::steady_clock::time_point::max();
    for (const auto& [key, group] : rendezvous_) {
      for (const Parked& parked : group) {
        earliest = std::min(earliest, parked.deadline);
      }
    }
    reaper_cv_.wait_until(lock, earliest);
    if (stopping_) {
      break;
    }

    // Re-scan: wait_until also returns spuriously and on a fresh park.
    const auto now = std::chrono::steady_clock::now();
    std::vector<std::shared_ptr<ClientHandle>> expired;
    for (auto& [key, group] : rendezvous_) {
      std::erase_if(group, [&](const Parked& parked) {
        const bool gone =
            parked.deadline <= now || parked.client->disconnected();
        if (gone) {
          expired.push_back(parked.client);
        }
        return gone;
      });
    }
    if (expired.empty()) {
      continue;
    }
    // Closing a stream can re-enter the matchmaker via Disconnect().
    lock.unlock();
    for (const auto& client : expired) {
      LOG(INFO) << "Rendezvous timed out for '" << client->name()
                << "'; closing its stream";
      client->MarkDisconnected();
      client->CloseAfterFlush();
    }
    lock.lock();
  }
}

void Matchmaker::Disconnect(const std::shared_ptr<ClientHandle>& client) {
  client->MarkDisconnected();
  std::lock_guard lock(mutex_);
  for (auto& [key, group] : rendezvous_) {
    std::erase_if(
        group, [&](const Parked& parked) { return parked.client == client; });
  }
  std::erase_if(rendezvous_,
                [](const auto& entry) { return entry.second.empty(); });
}

void Matchmaker::Drain() {
  std::unique_lock lock(drain_mutex_);
  drain_cv_.wait(lock, [&] { return running_games_.load() == 0; });
}

}  // namespace tournament_broker
