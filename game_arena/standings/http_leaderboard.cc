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
#include "absl/strings/str_cat.h"
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

// A silent client must not wedge the one serve coroutine.
constexpr auto kClientTimeout = std::chrono::seconds(5);

// Error codes come back in the completion tuple instead of as exceptions.
constexpr auto kAsTuple = net::as_tuple(net::use_awaitable);

}  // namespace

// One pass: StrReplaceAll never rescans a substitution, so '&' is safe here.
std::string HtmlEscape(std::string_view s) {
  return absl::StrReplaceAll(s, {{"&", "&amp;"},
                                 {"<", "&lt;"},
                                 {">", "&gt;"},
                                 {"\"", "&quot;"},
                                 {"'", "&#39;"}});
}

std::string PageStart(std::string_view title, bool refresh) {
  return absl::StrCat(
      "<!DOCTYPE html><html><head><meta charset=\"utf-8\">",
      refresh ? "<meta http-equiv=\"refresh\" content=\"5\">" : "", "<title>",
      HtmlEscape(title),
      "</title><style>body{font-family:sans-serif;margin:2em}"
      "table{border-collapse:collapse}"
      "td,th{border:1px solid #ccc;padding:4px 10px;text-align:right}"
      "th{background:#eee}td.l{text-align:left}"
      "td.d,th.d{color:#666;font-size:90%}"
      "pre{background:#f6f6f6;padding:8px;overflow:auto}"
      "nav a{margin-right:1em}</style></head><body>"
      "<nav><a href=\"/\">Leaderboard</a><a href=\"/jobs\">Jobs</a>"
      "<a href=\"/games\">Games</a></nav><h1>",
      HtmlEscape(title), "</h1>");
}

HttpLeaderboard::HttpLeaderboard(
    int port, const GameHistory *history,
    const tournament_arena::CandidateView *candidates,
    const tournament_arena::Standings *standings, std::string problem_name,
    Routes more)
    : port_(port),
      history_(history),
      candidates_(candidates),
      standings_(standings),
      problem_name_(std::move(problem_name)),
      more_(std::move(more)) {}

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
    // Or bound_port() would answer for a server that never started.
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
  // Posted, so the close runs on the io thread instead of racing async_accept.
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
  // Without them 404, not an empty table that looks like nobody has scored.
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
  return more_ ? more_(target) : std::nullopt;
}

std::string HttpLeaderboard::RenderLeaderboardHtml() const {
  const std::string title =
      problem_name_.empty() ? "Leaderboard" : problem_name_;
  const std::string score = standings_->score_label();
  // Graded rows show the host that measured it: what a reader must trust.
  const bool graded = standings_->graded();
  const std::vector<Standing> rows = standings_->Rank(0);
  const bool rated = !rows.empty() && rows.front().mu.has_value();
  const std::set<std::string> pool = pool_ ? pool_() : std::set<std::string>{};

  const auto table = [&](const std::vector<const Standing *> &section,
                         int first_rank) {
    std::ostringstream html;
    html << "<table><tr><th>Rank</th><th>Submission</th>"
            "<th>Author</th><th>"
         << HtmlEscape(score) << "</th>"
         << (rated ? "<th>mu</th><th>sigma</th>" : "")
         << (graded ? "<th>runs</th><th class=\"d\">machine</th>"
                    : "<th>W</th><th>D</th><th>L</th>")
         << "</tr>";
    int rank = first_rank;
    for (const Standing *row : section) {
      // Without a registry, the player name is its own display name; with
      // matchmaking every version of a participant shares one, so the id.
      const auto candidate = candidates_ != nullptr
                                 ? candidates_->Get(row->candidate_id)
                                 : std::nullopt;
      const std::string name = candidate.has_value() && !pool_
                                   ? candidate->display_name()
                                   : row->candidate_id;
      const std::string author =
          candidate.has_value() ? candidate->author() : std::string("-");
      html << "<tr><td>" << rank++
           << "</td><td class=\"l\"><a href=\"/participants/"
           << HtmlEscape(row->candidate_id) << "\">" << HtmlEscape(name)
           << "</a></td><td class=\"l\">" << HtmlEscape(author) << "</td><td>"
           << absl::StrFormat("%.*f", graded ? 3 : 1, row->score) << "</td>";
      if (rated) {
        html << "<td>" << absl::StrFormat("%.1f", row->mu.value_or(0))
             << "</td><td>" << absl::StrFormat("%.2f", row->sigma.value_or(0))
             << "</td>";
      }
      if (graded) {
        html << "<td>" << row->runs << "</td><td class=\"d l\">"
             << HtmlEscape(row->machine_class.empty() ? "-"
                                                      : row->machine_class)
             << "</td>";
      } else {
        html << "<td>" << row->wins << "</td><td>" << row->draws << "</td><td>"
             << row->losses << "</td>";
      }
      html << "</tr>";
    }
    html << "</table>";
    return html.str();
  };

  std::ostringstream html;
  html << PageStart(title, /*refresh=*/true);
  if (pool_) {
    // The pool keeps playing; the rest dropped out and keep their rating.
    std::vector<const Standing *> in, out;
    for (const Standing &row : rows) {
      (pool.contains(row.candidate_id) ? in : out).push_back(&row);
    }
    html << "<p>The pool: its ratings are still being measured, "
            "continuously (<a href=\"/pool\">matches and charts</a>).</p>"
         << table(in, 1);
    if (!out.empty()) {
      html << "<h2>Dropped out</h2><p>No longer matched; rated as they "
              "left.</p>"
           << table(out, static_cast<int>(in.size()) + 1);
    }
  } else {
    std::vector<const Standing *> all;
    for (const Standing &row : rows) {
      all.push_back(&row);
    }
    html << table(all, 1);
  }
  html << "<p><a href=\"/api/leaderboard\">JSON</a> &middot; "
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
  const std::set<std::string> pool = pool_ ? pool_() : std::set<std::string>{};
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
    if (row.mu.has_value()) {
      rows.back().as_object()["mu"] = *row.mu;
      rows.back().as_object()["sigma"] = *row.sigma;
    }
    if (pool_) {
      rows.back().as_object()["pool"] = pool.contains(row.candidate_id);
    }
  }
  return json::serialize(json::object{
      {"score_label", standings_->score_label()},
      {"rows", std::move(rows)},
  });
}

std::string HttpLeaderboard::RenderGamesJson() const {
  json::array games;
  for (const std::string &line : history_->RecentGames(100)) {
    // Re-parsed, so an unreadable line drops out rather than corrupting all.
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
