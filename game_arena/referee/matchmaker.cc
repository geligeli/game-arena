#include "game_arena/referee/matchmaker.h"

#include <algorithm>
#include <array>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/log/log.h"
#include "game_arena/referee/game_registry.h"

namespace tournament_broker {

namespace {

constexpr std::string_view kBuiltinPrefix = "builtin:";
constexpr std::string_view kPlayerPrefix = "player:";

// Order-independent, so both sides of a pairing compute the same key.
std::string PairingKey(const std::string& game, const std::string& a,
                       const std::string& b) {
  const std::string& lo = a < b ? a : b;
  const std::string& hi = a < b ? b : a;
  return game + "\t" + lo + "\t" + hi;
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
    StartPairedGame(descriptor, std::move(remote), std::move(bot));
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
           "' (expected: builtin:<spec> | player:<name>)";
  return false;
}

bool Matchmaker::JoinRendezvous(std::shared_ptr<ClientHandle> client,
                                const std::string& game,
                                const std::string& wanted, std::string* error) {
  const std::string key = PairingKey(game, client->name(), wanted);
  std::shared_ptr<ClientHandle> partner;
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
      rendezvous_.emplace(key,
                          Parked{.client = std::move(client),
                                 .game = game,
                                 .deadline = std::chrono::steady_clock::now() +
                                             config_.rendezvous_timeout});
      reaper_cv_.notify_all();
      return true;
    }
    // Same key and same name: a duplicate connection, not a pairing.
    if (it->second.client->name() == client->name()) {
      *error = "another connection is already waiting as '" + client->name() +
               "' for '" + wanted + "'";
      return false;
    }
    partner = it->second.client;
    rendezvous_.erase(it);
  }

  Seat waiting{.display_name = partner->name(),
               .client = std::move(partner),
               .builtin = nullptr};
  Seat arriving{.display_name = client->name(),
                .client = std::move(client),
                .builtin = nullptr};
  StartPairedGame(GameRegistry().at(game), std::move(waiting),
                  std::move(arriving));
  return true;
}

void Matchmaker::StartPairedGame(const GameDescriptor& descriptor, Seat a,
                                 Seat b) {
  const std::string key =
      PairingKey(descriptor.name, a.display_name, b.display_name);
  uint64_t played = 0;
  {
    std::lock_guard lock(mutex_);
    played = pairing_games_[key]++;
  }
  // By name, then by the pairing's game count: neither who arrived first nor
  // which side is a builtin decides who moves first.
  if ((a.display_name > b.display_name) != (played % 2 == 1)) {
    std::swap(a, b);
  }
  StartGame(descriptor, std::move(a), std::move(b));
}

void Matchmaker::StartGame(const GameDescriptor& descriptor, Seat seat0,
                           Seat seat1) {
  GameRunConfig run_config;
  run_config.turn_timeout = config_.turn_timeout;
  run_config.on_record = config_.on_record;
  run_config.game_time_budget = config_.game_time_budget;
  run_config.max_moves_per_game = config_.max_moves_per_game;

  const uint64_t id = ++game_counter_;
  ++running_games_;
  auto run = std::make_shared<GameRun>(
      descriptor, run_config,
      std::array<Seat, 2>{std::move(seat0), std::move(seat1)}, id, history_,
      &pool_, &timer_, [this, id] {
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
    for (auto& [key, parked] : rendezvous_) {
      waiting.push_back(parked.client);
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
    if (rendezvous_.empty()) {
      reaper_cv_.wait(lock);
      continue;
    }
    const auto earliest =
        std::min_element(rendezvous_.begin(), rendezvous_.end(),
                         [](const auto& a, const auto& b) {
                           return a.second.deadline < b.second.deadline;
                         })
            ->second.deadline;
    reaper_cv_.wait_until(lock, earliest);
    if (stopping_) {
      break;
    }

    // Re-scan: wait_until also returns spuriously and on a fresh park.
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
  std::erase_if(rendezvous_, [&](const auto& entry) {
    return entry.second.client == client;
  });
}

void Matchmaker::Drain() {
  std::unique_lock lock(drain_mutex_);
  drain_cv_.wait(lock, [&] { return running_games_.load() == 0; });
}

}  // namespace tournament_broker
