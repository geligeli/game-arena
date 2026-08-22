#ifndef RISK_GAME_AI_CPP_TOURNAMENT_SERVER_MATCHMAKER_H
#define RISK_GAME_AI_CPP_TOURNAMENT_SERVER_MATCHMAKER_H

// Matchmaking and game execution. Clients join with a Hello; "builtin:<spec>"
// opponents start immediately, "any" opponents queue FIFO per game, and
// "player:<name>" opponents rendezvous with that one named partner. Every game
// runs on its own thread: chance nodes are resolved server-side, built-in
// seats move inline, remote seats get YourTurn + a wall-clock deadline and
// lose on timeout/disconnect/illegal action.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>

#include "cpp/tournament_server/client_handle.h"
#include "cpp/tournament_server/elo_store.h"
#include "cpp/tournament_server/game_history.h"
#include "cpp/tournament_server/game_run.h"
#include "cpp/tournament_server/game_session.h"
#include "cpp/tournament_server/tournament_broker.pb.h"
#include "cpp/tournament_server/worker_pool.h"

namespace tournament_broker {

struct MatchmakerConfig {
  std::chrono::milliseconds turn_timeout{10000};
  // Total wall-clock thinking time one seat may spend across a whole game.
  // Zero disables the budget and leaves only the per-turn timeout, which on
  // its own bounds nothing: a slow strategy can burn turn_timeout on every one
  // of thousands of moves.
  std::chrono::milliseconds game_time_budget{0};
  // How long a "player:<name>" client waits for its named partner before its
  // stream is closed. Without it a partner that never builds, or crashes on
  // startup, parks the other side forever.
  std::chrono::milliseconds rendezvous_timeout{60000};
  int max_moves_per_game = 50000;
  // Threads serving all games. Games are state machines rather than threads,
  // so this bounds only how much CPU-heavy built-in work runs at once.
  int worker_threads = 0;  // <= 0: hardware_concurrency()
};

class Matchmaker {
 public:
  Matchmaker(MatchmakerConfig config, EloStore *elo_store,
             GameHistory *history);
  ~Matchmaker();

  Matchmaker(const Matchmaker &) = delete;
  auto operator=(const Matchmaker &) -> Matchmaker & = delete;

  // Queues the client ("any"), parks it for a named partner
  // ("player:<name>"), or starts a game against a built-in ("builtin:<spec>")
  // immediately. Returns false with *error set when the game, builtin spec or
  // partner name is unusable. Non-blocking: games run on their own threads.
  auto Join(std::shared_ptr<ClientHandle> client, const proto::Hello &hello,
            std::string *error) -> bool;

  // Dequeues the client if still waiting (in either the "any" queue or a
  // rendezvous slot); marks it disconnected so a running game awards the win
  // to the opponent.
  void Disconnect(const std::shared_ptr<ClientHandle> &client);

  // Refuses further joins, releases everyone still waiting, and aborts every
  // running game. Without this, Drain() waits out a full turn timeout for each
  // game parked on a client that will never answer. Idempotent.
  void Shutdown();

  // Blocks until all running games finish (used at shutdown and by tests).
  // Pair with Shutdown() to make it bounded.
  void Drain();

  auto running_games() const -> int { return running_games_.load(); }

  // Clients currently waiting for any opponent in |game|.
  auto queued(const std::string &game) const -> int;

  // Clients currently parked for a specific named partner in |game|.
  auto parked(const std::string &game) const -> int;

 private:
  // A client waiting for one specific partner to show up.
  struct Parked {
    std::shared_ptr<ClientHandle> client;
    std::string game;
    std::chrono::steady_clock::time_point deadline;
  };

  void MaybeStartGame(const std::string &game);
  // Hands two seats to a self-owning GameRun and counts it as running until it
  // concludes. Seat 0 moves first.
  void StartGame(const GameDescriptor &descriptor, Seat seat0, Seat seat1);
  // Handles a "player:<name>" join: parks, or pairs with an already-parked
  // partner. Precondition: |wanted| is non-empty and differs from the client's
  // own name.
  auto JoinRendezvous(std::shared_ptr<ClientHandle> client,
                      const std::string &game, const std::string &wanted,
                      std::string *error) -> bool;
  // Closes rendezvous slots whose deadline has passed. Runs until stopping_.
  void ReaperLoop();

  const MatchmakerConfig config_;
  EloStore *elo_store_;   // not owned
  GameHistory *history_;  // not owned

  mutable std::mutex mutex_;  // guards queues_, rendezvous_*, stopping_
  std::map<std::string, std::deque<std::weak_ptr<ClientHandle>>> queues_;
  // Key: game + '\t' + the two player names, sorted. Both sides therefore
  // derive the same key, and a key collision already implies each side named
  // the other.
  std::map<std::string, Parked> rendezvous_;
  // Games already played per rendezvous key, so a series of games between the
  // same two players alternates seats regardless of who parks first. Grows by
  // one entry per pair that has actually played.
  std::map<std::string, uint64_t> rendezvous_games_;
  // Weak on purpose: a GameRun keeps itself alive until it concludes, so this
  // registry exists only to reach in-flight games at shutdown and must never
  // be what keeps one alive.
  std::map<uint64_t, std::weak_ptr<GameRun>> running_;
  bool stopping_ = false;
  std::condition_variable reaper_cv_;

  std::atomic<int> running_games_{0};
  std::atomic<uint64_t> game_counter_{0};
  std::mutex drain_mutex_;
  std::condition_variable drain_cv_;

  // Declared before reaper_ so they outlive it, and stopped explicitly in the
  // destructor after the reaper has been joined.
  WorkerPool pool_;
  Timer timer_;
  std::thread reaper_;
};

}  // namespace tournament_broker

#endif  // RISK_GAME_AI_CPP_TOURNAMENT_SERVER_MATCHMAKER_H
