#include "game_arena/standings/http_leaderboard.h"

#include <boost/asio/as_tuple.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/json/array.hpp>
#include <boost/json/object.hpp>
#include <boost/json/parse.hpp>
#include <boost/json/serialize.hpp>
#include <boost/json/value.hpp>
#include <chrono>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

#include "absl/log/log.h"
#include "absl/strings/str_format.h"
#include "absl/strings/str_replace.h"

namespace tournament_broker {

using tournament_arena::Standing;

namespace beast = boost::beast;
namespace http = beast::http;
namespace json = boost::json;
namespace net = boost::asio;
using tcp = net::ip::tcp;

namespace {

// A client that connects and then goes silent must not wedge the single serve
// coroutine, which takes one connection at a time: bound every operation on it.
constexpr auto kClientTimeout = std::chrono::seconds(5);

// Error codes come back in the completion tuple instead of as exceptions.
constexpr auto kAsTuple = net::as_tuple(net::use_awaitable);

// One pass, so the order of these pairs does not matter: StrReplaceAll never
// rescans what it just substituted, which is what makes escaping '&' safe
// alongside the entities that contain one.
std::string HtmlEscape(std::string_view s) {
  return absl::StrReplaceAll(s, {{"&", "&amp;"},
                                 {"<", "&lt;"},
                                 {">", "&gt;"},
                                 {"\"", "&quot;"},
                                 {"'", "&#39;"}});
}

}  // namespace

HttpLeaderboard::HttpLeaderboard(
    int port, const GameHistory *history,
    const tournament_arena::CandidateView *candidates,
    const tournament_arena::Standings *standings, std::string problem_name)
    : port_(port),
      history_(history),
      candidates_(candidates),
      standings_(standings),
      problem_name_(std::move(problem_name)) {}

HttpLeaderboard::~HttpLeaderboard() { Stop(); }

bool HttpLeaderboard::Start() {
  const tcp::endpoint endpoint(tcp::v4(), static_cast<unsigned short>(port_));
  beast::error_code ec;
  acceptor_.open(endpoint.protocol(), ec);
  if (!ec) acceptor_.set_option(net::socket_base::reuse_address(true), ec);
  if (!ec) acceptor_.bind(endpoint, ec);
  if (!ec) acceptor_.listen(net::socket_base::max_listen_connections, ec);
  if (ec) {
    LOG(ERROR) << "HTTP leaderboard: cannot bind port " << port_ << ": "
               << ec.message();
    // open() may well have succeeded; leaving the acceptor open would make
    // bound_port() answer 0 for a server that never started.
    beast::error_code ignored;
    acceptor_.close(ignored);
    return false;
  }
  net::co_spawn(ioc_, Serve(), net::detached);
  thread_ = std::thread([this] { ioc_.run(); });
  return true;
}

int HttpLeaderboard::bound_port() const {
  beast::error_code ec;
  const tcp::endpoint endpoint = acceptor_.local_endpoint(ec);
  return ec ? -1 : endpoint.port();
}

void HttpLeaderboard::Stop() {
  if (!thread_.joinable()) {
    return;
  }
  // Closing the acceptor here would race the coroutine sitting in
  // async_accept; posting it runs the close on the io thread instead, where
  // the pending accept then completes with an error. A connection already
  // being served finishes first, bounded by kClientTimeout.
  net::post(ioc_, [this] {
    beast::error_code ignored;
    acceptor_.close(ignored);
  });
  thread_.join();
}

net::awaitable<void> HttpLeaderboard::Serve() {
  for (;;) {
    auto [accept_ec, socket] = co_await acceptor_.async_accept(kAsTuple);
    if (accept_ec) {
      co_return;  // Stop() closed the acceptor.
    }
    beast::tcp_stream stream(std::move(socket));
    stream.expires_after(kClientTimeout);

    beast::flat_buffer buffer;
    http::request<http::string_body> request;
    [[maybe_unused]] auto [read_ec, read_bytes] =
        co_await http::async_read(stream, buffer, request, kAsTuple);
    if (read_ec) {
      continue;
    }

    http::response<http::string_body> response;
    response.version(request.version());
    response.keep_alive(false);
    const auto target = request.target();
    if (request.method() != http::verb::get) {
      response.result(http::status::method_not_allowed);
      response.set(http::field::content_type, "text/plain");
      response.body() = "GET only\n";
    } else if (auto routed = Route({target.data(), target.size()})) {
      response.result(http::status::ok);
      response.set(http::field::content_type, routed->first);
      response.body() = std::move(routed->second);
    } else {
      response.result(http::status::not_found);
      response.set(http::field::content_type, "text/plain");
      response.body() = "not found\n";
    }
    response.prepare_payload();

    [[maybe_unused]] auto [write_ec, write_bytes] =
        co_await http::async_write(stream, response, kAsTuple);
    beast::error_code ignored;
    stream.socket().shutdown(tcp::socket::shutdown_send, ignored);
  }
}

std::optional<std::pair<std::string, std::string>> HttpLeaderboard::Route(
    std::string_view target) const {
  constexpr std::string_view kHtml = "text/html; charset=utf-8";
  constexpr std::string_view kJson = "application/json";
  // The standings and candidate store come from the arena. A standalone broker
  // has neither, and 404 is the honest answer there rather than an empty table
  // that looks like nobody has scored yet.
  if ((target == "/" || target == "/index.html") && standings_ != nullptr) {
    return std::pair(std::string(kHtml), RenderLeaderboardHtml());
  }
  if (target == "/api/leaderboard" && standings_ != nullptr) {
    return std::pair(std::string(kJson), RenderLeaderboardJson());
  }
  if (target == "/api/games") {
    return std::pair(std::string(kJson), RenderGamesJson());
  }
  if (target == "/api/candidates" && candidates_ != nullptr) {
    return std::pair(std::string(kJson), RenderCandidatesJson());
  }
  return std::nullopt;
}

std::string HttpLeaderboard::RenderLeaderboardHtml() const {
  const std::string title =
      problem_name_.empty() ? "Leaderboard" : problem_name_;
  const std::string score = standings_->score_label();

  std::ostringstream html;
  html << "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
          "<meta http-equiv=\"refresh\" content=\"5\">"
          "<title>"
       << HtmlEscape(title)
       << "</title>"
          "<style>body{font-family:sans-serif;margin:2em}"
          "table{border-collapse:collapse}"
          "td,th{border:1px solid #ccc;padding:4px 10px;text-align:right}"
          "th{background:#eee}td.l{text-align:left}"
          "td.d,th.d{color:#666;font-size:90%}</style></head><body>"
          "<h1>"
       << HtmlEscape(title)
       << "</h1><table><tr><th>Rank</th><th>Submission</th>"
          "<th>Author</th><th>"
       << HtmlEscape(score) << "</th>";
  // A match problem has a W/D/L record; a graded one has the host that produced
  // the number, which is the thing a reader most needs to trust it.
  const bool graded = score != "elo";
  html << (graded ? "<th>runs</th><th class=\"d\">machine</th>"
                  : "<th>W</th><th>D</th><th>L</th>");
  html << "</tr>";

  int rank = 1;
  for (const Standing &row : standings_->Rank(0)) {
    // Without a submission registry -- the standalone broker's case -- a row is
    // just a player name, which is its own display name.
    const auto candidate = candidates_ != nullptr
                               ? candidates_->Get(row.candidate_id)
                               : std::nullopt;
    const std::string name =
        candidate.has_value() ? candidate->display_name() : row.candidate_id;
    const std::string author =
        candidate.has_value() ? candidate->author() : std::string("-");
    html << "<tr><td>" << rank++ << "</td><td class=\"l\">" << HtmlEscape(name)
         << "</td><td class=\"l\">" << HtmlEscape(author) << "</td><td>"
         << absl::StrFormat("%.*f", graded ? 3 : 1, row.score) << "</td>";
    if (graded) {
      html << "<td>" << row.runs << "</td><td class=\"d l\">"
           << HtmlEscape(row.machine_class.empty() ? "-" : row.machine_class)
           << "</td>";
    } else {
      html << "<td>" << row.wins << "</td><td>" << row.draws << "</td><td>"
           << row.losses << "</td>";
    }
    html << "</tr>";
  }
  html << "</table><p><a href=\"/api/leaderboard\">JSON</a> &middot; "
          "<a href=\"/api/candidates\">candidates</a> &middot; "
          "<a href=\"/api/games\">recent games</a></p></body></html>";
  return html.str();
}

std::string HttpLeaderboard::RenderCandidatesJson() const {
  json::array candidates;
  for (const auto &candidate : candidates_->List()) {
    const Standing row = standings_ != nullptr
                             ? standings_->Get(candidate.candidate_id())
                             : Standing{};
    candidates.push_back(json::object{
        {"candidate_id", candidate.candidate_id()},
        {"display_name", candidate.display_name()},
        {"author", candidate.author()},
        {"game", candidate.game()},
        {"parent_id", candidate.parent_id()},
        {"status",
         tournament_arena::proto::Candidate::Status_Name(candidate.status())},
        {"score", row.score},
        {"wins", row.wins},
        {"draws", row.draws},
        {"losses", row.losses},
        {"submitted_unix_ms", candidate.submitted_unix_ms()},
    });
  }
  return json::serialize(candidates);
}

std::string HttpLeaderboard::RenderLeaderboardJson() const {
  json::array rows;
  int rank = 1;
  for (const Standing &row : standings_->Rank(0)) {
    const auto candidate = candidates_ != nullptr
                               ? candidates_->Get(row.candidate_id)
                               : std::nullopt;
    json::object metrics;
    for (const auto &[name, value] : row.metrics) {
      metrics[name] = value;
    }
    rows.push_back(json::object{
        {"rank", rank++},
        {"player", row.candidate_id},
        {"candidate_id", row.candidate_id},
        {"display_name",
         candidate.has_value() ? candidate->display_name() : row.candidate_id},
        {"author",
         candidate.has_value() ? candidate->author() : std::string("-")},
        {"score", row.score},
        {"wins", row.wins},
        {"draws", row.draws},
        {"losses", row.losses},
        {"runs", row.runs},
        {"worker_id", row.worker_id},
        {"machine_class", row.machine_class},
        {"metrics", std::move(metrics)},
    });
  }
  return json::serialize(json::object{
      {"score_label", standings_->score_label()},
      {"rows", std::move(rows)},
  });
}

std::string HttpLeaderboard::RenderGamesJson() const {
  json::array games;
  for (const std::string &line : history_->RecentGames(100)) {
    // Each index line is already a JSON object. Re-parsing rather than
    // concatenating costs little at this size and means one unreadable line
    // drops out on its own instead of corrupting the whole document.
    boost::system::error_code ec;
    json::value game = json::parse(line, ec);
    if (ec) {
      LOG(WARNING) << "HTTP leaderboard: skipping unparseable game index line: "
                   << ec.message();
      continue;
    }
    games.push_back(std::move(game));
  }
  return json::serialize(games);
}

}  // namespace tournament_broker
