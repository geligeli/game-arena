#ifndef GAME_MCTS_GAME_MCTS_TOURNAMENT_SERVER_SERVER_HTTP_LEADERBOARD_H
#define GAME_MCTS_GAME_MCTS_TOURNAMENT_SERVER_SERVER_HTTP_LEADERBOARD_H

// Minimal embedded HTTP server (GET only) exposing the leaderboard.
//
// It renders whatever the problem scores by without knowing which that is: the
// score column's heading comes from the standings, so a graded problem shows
// "wall_ms" where a match problem shows "elo", and neither needs a branch here.
//
//   GET /                 HTML table of ratings
//   GET /api/leaderboard  same as JSON
//   GET /api/games        recent games (JSON array, from the history index)
//   GET /api/candidates   submitted strategies and their status (JSON array)
// Hand-rolled over POSIX sockets: one accept thread, ~100 lines, no extra
// dependency.
//
// Read-only on purpose. Submitting a candidate or scheduling a match goes
// through the Arena gRPC service, so there is exactly one write path to
// secure later.

#include <atomic>
#include <string>
#include <thread>

#include "game_mcts/tournament_server/server/candidate_store.h"
#include "game_mcts/tournament_server/server/game_history.h"
#include "game_mcts/tournament_server/server/standings.h"

namespace tournament_broker {

class HttpLeaderboard {
 public:
  // |candidates| and |standings| may be null, in which case /api/candidates and
  // /api/leaderboard 404: the standalone broker has neither and is still
  // usable without them.
  HttpLeaderboard(int port, const GameHistory *history,
                  const tournament_arena::CandidateStore *candidates = nullptr,
                  const tournament_arena::Standings *standings = nullptr,
                  std::string problem_name = "");
  ~HttpLeaderboard();

  // Starts the accept thread. Returns false when the port cannot be bound.
  auto Start() -> bool;
  void Stop();

  // The actual bound port (after Start); useful when constructed with port 0.
  auto bound_port() const -> int;

 private:
  void ServeLoop();
  void HandleConnection(int fd);
  auto RenderLeaderboardHtml() const -> std::string;
  auto RenderLeaderboardJson() const -> std::string;
  auto RenderGamesJson() const -> std::string;
  auto RenderCandidatesJson() const -> std::string;

  const int port_;
  const GameHistory *history_;  // not owned
  const tournament_arena::CandidateStore
      *candidates_;                               // not owned, may be null
  const tournament_arena::Standings *standings_;  // not owned, may be null
  const std::string problem_name_;
  // Atomic: Stop() runs on the caller's thread while ServeLoop() sits in
  // accept(). Stop() only shuts the socket down to wake accept(); the close()
  // happens after the serve thread is joined, so the descriptor can never be
  // closed (and its number reused) while accept() still holds it.
  std::atomic<int> listen_fd_{-1};
  std::atomic<bool> stop_{false};
  std::thread thread_;
};

}  // namespace tournament_broker

#endif  // GAME_MCTS_GAME_MCTS_TOURNAMENT_SERVER_SERVER_HTTP_LEADERBOARD_H
