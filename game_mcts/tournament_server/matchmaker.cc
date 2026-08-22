#include "cpp/tournament_server/matchmaker.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/cleanup/cleanup.h"
#include "absl/log/log.h"
#include "cpp/tournament_server/game_registry.h"

namespace tournament_broker {

namespace {

constexpr std::string_view kBuiltinPrefix = "builtin:";
constexpr std::string_view kPlayerPrefix = "player:";

// Order-independent key for a pair of players in one game, so both sides of a
// "player:<name>" rendezvous compute the same string.
auto RendezvousKey(const std::string &game, const std::string &a,
                   const std::string &b) -> std::string {
  const std::string &lo = a < b ? a : b;
  const std::string &hi = a < b ? b : a;
  return game + "\t" + lo + "\t" + hi;
}

}  // namespace

Matchmaker::Matchmaker(MatchmakerConfig config, EloStore *elo_store,
                       GameHistory *history)
    : config_(config),
      elo_store_(elo_store),
      history_(history),
      pool_(config.worker_threads > 0
                ? config.worker_threads
                : static_cast<int>(std::thread::hardware_concurrency())) {
  reaper_ = std::thread(&Matchmaker::ReaperLoop, this);
}

Matchmaker::~Matchmaker() {
  std::vector<std::shared_ptr<ClientHandle>> stranded;
  {
    std::lock_guard lock(mutex_);
    stopping_ = true;
    for (auto &[key, entry] : rendezvous_) {
      stranded.push_back(entry.client);
    }
    rendezvous_.clear();
  }
  reaper_cv_.notify_all();
  if (reaper_.joinable()) {
    reaper_.join();
  }
  for (const auto &client : stranded) {
    client->MarkDisconnected();
    client->CloseAfterFlush();
  }
  // Deadlines first: a pending turn timer would otherwise post onto a strand
  // whose pool is already draining.
  timer_.Stop();
  pool_.Stop();
}

auto Matchmaker::Join(std::shared_ptr<ClientHandle> client,
                      const proto::Hello &hello, std::string *error) -> bool {
  {
    std::lock_guard lock(mutex_);
    if (stopping_) {
      // Otherwise a client arriving during shutdown would be queued or started
      // after Shutdown() had already swept, and nothing would clean it up.
      *error = "server is shutting down";
      return false;
    }
  }

  const auto &registry = GameRegistry();
  const auto it = registry.find(hello.game());
  if (it == registry.end()) {
    *error = "unknown game '" + hello.game() + "'";
    return false;
  }
  const GameDescriptor &descriptor = it->second;

  if (hello.opponent().empty() || hello.opponent() == "any") {
    {
      std::lock_guard lock(mutex_);
      queues_[hello.game()].push_back(client);
    }
    MaybeStartGame(hello.game());
    return true;
  }

  if (hello.opponent().substr(0, kBuiltinPrefix.size()) == kBuiltinPrefix) {
    const std::string_view spec =
        std::string_view(hello.opponent()).substr(kBuiltinPrefix.size());
    auto builtin = descriptor.make_builtin(spec, error);
    if (!builtin.has_value()) {
      return false;
    }
    Seat remote{.display_name = client->name(),
                .client = std::move(client),
                .builtin = nullptr};
    Seat bot{.display_name = std::string(kBuiltinPrefix) + std::string(spec),
             .client = nullptr,
             .builtin = std::move(*builtin)};
    StartGame(descriptor, std::move(remote), std::move(bot));
    return true;
  }

  if (hello.opponent().substr(0, kPlayerPrefix.size()) == kPlayerPrefix) {
    const std::string wanted = hello.opponent().substr(kPlayerPrefix.size());
    if (wanted.empty()) {
      *error = "empty partner name in opponent 'player:'";
      return false;
    }
    if (wanted == client->name()) {
      *error = "'" + wanted + "' cannot play itself";
      return false;
    }
    return JoinRendezvous(std::move(client), hello.game(), wanted, error);
  }

  *error = "unknown opponent '" + hello.opponent() +
           "' (expected: any | builtin:<spec> | player:<name>)";
  return false;
}

auto Matchmaker::JoinRendezvous(std::shared_ptr<ClientHandle> client,
                                const std::string &game,
                                const std::string &wanted, std::string *error)
    -> bool {
  const std::string key = RendezvousKey(game, client->name(), wanted);
  std::shared_ptr<ClientHandle> partner;
  uint64_t pair_games = 0;
  {
    std::lock_guard lock(mutex_);
    if (stopping_) {
      *error = "server is shutting down";
      return false;
    }
    auto it = rendezvous_.find(key);
    if (it != rendezvous_.end() && it->second.client->disconnected()) {
      rendezvous_.erase(it);
      it = rendezvous_.end();
    }
    if (it == rendezvous_.end()) {
      rendezvous_.emplace(
          key, Parked{.client = std::move(client),
                      .game = game,
                      .deadline = std::chrono::steady_clock::now() +
                                  config_.rendezvous_timeout});
      reaper_cv_.notify_all();
      return true;
    }
    // The key is the sorted name pair, so reaching here already means each
    // side named the other -- unless both sides are the same player, which is
    // a duplicate connection rather than a pairing.
    if (it->second.client->name() == client->name()) {
      *error = "another connection is already waiting as '" + client->name() +
               "' for '" + wanted + "'";
      return false;
    }
    partner = it->second.client;
    rendezvous_.erase(it);
    pair_games = rendezvous_games_[key]++;
  }

  const GameDescriptor &descriptor = GameRegistry().at(game);
  Seat waiting{.display_name = partner->name(),
               .client = std::move(partner),
               .builtin = nullptr};
  Seat arriving{.display_name = client->name(),
                .client = std::move(client),
                .builtin = nullptr};
  // Alternate seats across the pair's series: who parked first is a race
  // between two sandbox workers, so it must not decide who moves first.
  if (pair_games % 2 == 0) {
    StartGame(descriptor, std::move(waiting), std::move(arriving));
  } else {
    StartGame(descriptor, std::move(arriving), std::move(waiting));
  }
  return true;
}

void Matchmaker::StartGame(const GameDescriptor &descriptor, Seat seat0,
                           Seat seat1) {
  GameRunConfig run_config;
  run_config.turn_timeout = config_.turn_timeout;
  run_config.game_time_budget = config_.game_time_budget;
  run_config.max_moves_per_game = config_.max_moves_per_game;

  const uint64_t id = ++game_counter_;
  ++running_games_;
  auto run = GameRun::Create(
      descriptor, run_config,
      std::array<Seat, 2>{std::move(seat0), std::move(seat1)}, id, elo_store_,
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
  // GameRun keeps itself alive until it concludes; the registry above is weak
  // and exists only so Shutdown() can reach it.
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
    // Release everyone still waiting for an opponent: nothing will ever pair
    // them now, and their streams would otherwise stay open until the client
    // gave up.
    for (auto &[game, queue] : queues_) {
      for (const std::weak_ptr<ClientHandle> &weak : queue) {
        if (auto client = weak.lock()) {
          waiting.push_back(std::move(client));
        }
      }
    }
    queues_.clear();
    for (auto &[key, parked] : rendezvous_) {
      waiting.push_back(parked.client);
    }
    rendezvous_.clear();
    for (auto &[id, weak] : running_) {
      if (auto run = weak.lock()) {
        games.push_back(std::move(run));
      }
    }
  }
  reaper_cv_.notify_all();

  for (const std::shared_ptr<ClientHandle> &client : waiting) {
    client->MarkDisconnected();
    client->CloseAfterFlush();
  }
  for (const std::shared_ptr<GameRun> &run : games) {
    run->Abort("server_shutdown");
  }
}

void Matchmaker::ReaperLoop() {
  std::unique_lock lock(mutex_);
  while (!stopping_) {
    if (rendezvous_.empty()) {
      reaper_cv_.wait(lock);
      continue;
    }
    const auto earliest =
        std::min_element(rendezvous_.begin(), rendezvous_.end(),
                         [](const auto &a, const auto &b) {
                           return a.second.deadline < b.second.deadline;
                         })
            ->second.deadline;
    reaper_cv_.wait_until(lock, earliest);
    if (stopping_) {
      break;
    }

    // Re-scan unconditionally: wait_until also returns on spurious wakeups and
    // on the notify from a fresh park.
    const auto now = std::chrono::steady_clock::now();
    std::vector<std::shared_ptr<ClientHandle>> expired;
    for (auto it = rendezvous_.begin(); it != rendezvous_.end();) {
      if (it->second.deadline <= now || it->second.client->disconnected()) {
        expired.push_back(it->second.client);
        it = rendezvous_.erase(it);
      } else {
        ++it;
      }
    }
    if (expired.empty()) {
      continue;
    }
    // Closing a stream can re-enter the matchmaker via Disconnect().
    lock.unlock();
    for (const auto &client : expired) {
      LOG(INFO) << "Rendezvous timed out for '" << client->name()
                << "'; closing its stream";
      client->MarkDisconnected();
      client->CloseAfterFlush();
    }
    lock.lock();
  }
}

void Matchmaker::Disconnect(const std::shared_ptr<ClientHandle> &client) {
  client->MarkDisconnected();
  std::lock_guard lock(mutex_);
  for (auto &[game, queue] : queues_) {
    std::erase_if(queue, [&](const std::weak_ptr<ClientHandle> &weak) {
      const auto stored = weak.lock();
      return !stored || stored == client;
    });
  }
  std::erase_if(rendezvous_, [&](const auto &entry) {
    return entry.second.client == client;
  });
}

auto Matchmaker::queued(const std::string &game) const -> int {
  std::lock_guard lock(mutex_);
  const auto it = queues_.find(game);
  return it == queues_.end() ? 0 : static_cast<int>(it->second.size());
}

auto Matchmaker::parked(const std::string &game) const -> int {
  std::lock_guard lock(mutex_);
  return static_cast<int>(std::count_if(
      rendezvous_.begin(), rendezvous_.end(),
      [&](const auto &entry) { return entry.second.game == game; }));
}

void Matchmaker::MaybeStartGame(const std::string &game) {
  std::array<std::shared_ptr<ClientHandle>, 2> pair;
  {
    std::lock_guard lock(mutex_);
    auto &queue = queues_[game];

    // Compact the queue, dropping entries whose client is gone, in FIFO order.
    std::vector<std::shared_ptr<ClientHandle>> live;
    live.reserve(queue.size());
    for (const std::weak_ptr<ClientHandle> &weak : queue) {
      auto client = weak.lock();
      if (client && !client->disconnected()) {
        live.push_back(std::move(client));
      }
    }

    // Earliest pair of *distinct* players. A player must not play themselves,
    // but same-named entries at the head must not block a valid pairing
    // further back: [alice, alice, bob] has to match alice with bob.
    size_t first = 0;
    size_t second = 0;
    bool found = false;
    for (size_t i = 0; i < live.size() && !found; ++i) {
      for (size_t j = i + 1; j < live.size(); ++j) {
        if (live[i]->name() != live[j]->name()) {
          first = i;
          second = j;
          found = true;
          break;
        }
      }
    }

    queue.clear();
    if (!found) {
      for (const auto &client : live) {
        queue.push_back(client);
      }
      return;
    }
    for (size_t i = 0; i < live.size(); ++i) {
      if (i != first && i != second) {
        queue.push_back(live[i]);
      }
    }
    pair[0] = std::move(live[first]);
    pair[1] = std::move(live[second]);
  }
  const GameDescriptor &descriptor = GameRegistry().at(game);
  Seat a{.display_name = pair[0]->name(),
         .client = std::move(pair[0]),
         .builtin = nullptr};
  Seat b{.display_name = pair[1]->name(),
         .client = std::move(pair[1]),
         .builtin = nullptr};
  StartGame(descriptor, std::move(a), std::move(b));
}

void Matchmaker::Drain() {
  std::unique_lock lock(drain_mutex_);
  drain_cv_.wait(lock, [&] { return running_games_.load() == 0; });
}

}  // namespace tournament_broker
