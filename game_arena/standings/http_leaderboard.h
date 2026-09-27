#ifndef GAME_ARENA_GAME_ARENA_STANDINGS_HTTP_LEADERBOARD_H
#define GAME_ARENA_GAME_ARENA_STANDINGS_HTTP_LEADERBOARD_H

// GET-only leaderboard: /, /api/leaderboard, /api/games, /api/candidates, and
// |more| for the rest. Read-only on purpose: writes go through the Arena gRPC.

#include <boost/asio/awaitable.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include "game_arena/standings/candidate_view.h"
#include "game_arena/standings/game_history.h"
#include "game_arena/standings/standings.h"

namespace tournament_broker {

// |s| with every character HTML treats specially escaped, quotes included.
std::string HtmlEscape(std::string_view s);

// Head, style, navigation and heading; |refresh| reloads every five seconds.
std::string PageStart(std::string_view title, bool refresh = false);

class HttpLeaderboard {
 public:
  // A content type and body for a GET of the target, or nullopt for a 404.
  using Routes =
      std::function<std::optional<std::pair<std::string, std::string>>(
          std::string_view target)>;

  // A null |candidates| or |standings| 404s its routes.
  HttpLeaderboard(int port, const GameHistory *history,
                  const tournament_arena::CandidateView *candidates = nullptr,
                  const tournament_arena::Standings *standings = nullptr,
                  std::string problem_name = "", Routes more = nullptr);
  ~HttpLeaderboard();

  // Starts the accept thread. Returns false when the port cannot be bound.
  bool Start();
  void Stop();

  // After Start(); useful with port 0.
  int bound_port() const;

 private:
  boost::asio::awaitable<void> Serve();

  // Strings rather than a response, so Beast's types stay in the .cc.
  std::optional<std::pair<std::string, std::string>> Route(
      std::string_view target) const;

  std::string RenderLeaderboardHtml() const;
  std::string RenderLeaderboardJson() const;
  std::string RenderGamesJson() const;
  std::string RenderCandidatesJson() const;

  const int port_;
  const GameHistory *history_;                         // not owned
  const tournament_arena::CandidateView *candidates_;  // not owned, may be null
  const tournament_arena::Standings *standings_;       // not owned, may be null
  const std::string problem_name_;
  const Routes more_;
  // Everything on the acceptor and its sockets runs on thread_.
  boost::asio::io_context ioc_{1};
  boost::asio::ip::tcp::acceptor acceptor_{ioc_};
  std::thread thread_;
};

}  // namespace tournament_broker

#endif  // GAME_ARENA_GAME_ARENA_STANDINGS_HTTP_LEADERBOARD_H
