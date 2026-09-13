#ifndef GAME_ARENA_GAME_ARENA_STANDINGS_HTTP_LEADERBOARD_H
#define GAME_ARENA_GAME_ARENA_STANDINGS_HTTP_LEADERBOARD_H

// Embedded HTTP server (GET only) exposing the leaderboard.
//
// It renders whatever the problem scores by without knowing which that is: the
// score column's heading comes from the standings, so a graded problem shows
// "wall_ms" where a match problem shows "elo", and neither needs a branch here.
//
//   GET /                 HTML table of ratings
//   GET /api/leaderboard  same as JSON
//   GET /api/games        recent games (JSON array, from the history index)
//   GET /api/candidates   submitted strategies and their status (JSON array)
//
// Boost.Beast over Boost.Asio owns the socket, the request parser and the
// response framing; one coroutine on one thread accepts and serves connections
// in turn, which is all a status page polled every five seconds needs.
//
// Read-only on purpose. Submitting a candidate or scheduling a match goes
// through the Arena gRPC service, so there is exactly one write path to
// secure later.

#include <boost/asio/awaitable.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include "game_arena/standings/candidate_view.h"
#include "game_arena/standings/game_history.h"
#include "game_arena/standings/standings.h"

namespace tournament_broker {

class HttpLeaderboard {
 public:
  // |candidates| and |standings| may be null, in which case /api/candidates and
  // /api/leaderboard 404: the standalone broker has neither and is still
  // usable without them.
  HttpLeaderboard(int port, const GameHistory *history,
                  const tournament_arena::CandidateView *candidates = nullptr,
                  const tournament_arena::Standings *standings = nullptr,
                  std::string problem_name = "");
  ~HttpLeaderboard();

  // Starts the accept thread. Returns false when the port cannot be bound.
  bool Start();
  void Stop();

  // The actual bound port (after Start); useful when constructed with port 0.
  int bound_port() const;

 private:
  // Accepts and serves connections until Stop() closes the acceptor.
  boost::asio::awaitable<void> Serve();

  // The content type and body for a GET of |target|, or nullopt for 404.
  // Routing returns strings rather than a response so that Beast's HTTP
  // message types stay inside the .cc.
  std::optional<std::pair<std::string, std::string>> Route(
      std::string_view target) const;

  std::string RenderLeaderboardHtml() const;
  std::string RenderLeaderboardJson() const;
  std::string RenderGamesJson() const;
  std::string RenderCandidatesJson() const;

  const int port_;
  const GameHistory *history_;  // not owned
  const tournament_arena::CandidateView
      *candidates_;                               // not owned, may be null
  const tournament_arena::Standings *standings_;  // not owned, may be null
  const std::string problem_name_;
  // Single-threaded: every operation on the acceptor and on accepted sockets
  // runs on thread_. Stop() reaches it by posting the close, so nothing touches
  // the acceptor from two threads at once.
  boost::asio::io_context ioc_{1};
  boost::asio::ip::tcp::acceptor acceptor_{ioc_};
  std::thread thread_;
};

}  // namespace tournament_broker

#endif  // GAME_ARENA_GAME_ARENA_STANDINGS_HTTP_LEADERBOARD_H
