#include "game_mcts/tournament_server/http_leaderboard.h"

#include <algorithm>
#include <cstdio>
#include <sstream>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "absl/log/log.h"

namespace tournament_broker {

namespace {

// A client that connects and then goes silent must not wedge the single
// accept/serve thread: bound both directions of every accepted connection.
constexpr int kClientTimeoutSeconds = 5;

void SetSocketTimeouts(int fd) {
  timeval tv{.tv_sec = kClientTimeoutSeconds, .tv_usec = 0};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}

auto HtmlEscape(const std::string &s) -> std::string {
  std::string out;
  out.reserve(s.size());
  for (const char c : s) {
    switch (c) {
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '&': out += "&amp;"; break;
      case '"': out += "&quot;"; break;
      default: out += c;
    }
  }
  return out;
}

auto JsonEscape(const std::string &s) -> std::string {
  std::string out;
  out.reserve(s.size());
  for (const char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default: out += c;
    }
  }
  return out;
}

struct LeaderboardRow {
  std::string game;
  std::string player;
  proto::Rating rating;
};

void WriteAll(int fd, const std::string &data) {
  size_t sent = 0;
  while (sent < data.size()) {
    const ssize_t n = send(fd, data.data() + sent, data.size() - sent, 0);
    if (n <= 0) {
      return;
    }
    sent += static_cast<size_t>(n);
  }
}

void Respond(int fd, int status, const std::string &status_text,
             const std::string &content_type, const std::string &body) {
  std::ostringstream head;
  head << "HTTP/1.1 " << status << ' ' << status_text << "\r\n"
       << "Content-Type: " << content_type << "\r\n"
       << "Content-Length: " << body.size() << "\r\n"
       << "Connection: close\r\n\r\n";
  WriteAll(fd, head.str());
  WriteAll(fd, body);
}

}  // namespace

HttpLeaderboard::HttpLeaderboard(
    int port, const EloStore *elo_store, const GameHistory *history,
    const tournament_arena::CandidateStore *candidates)
    : port_(port),
      elo_store_(elo_store),
      history_(history),
      candidates_(candidates) {}

HttpLeaderboard::~HttpLeaderboard() { Stop(); }

auto HttpLeaderboard::Start() -> bool {
  const int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    LOG(ERROR) << "HTTP leaderboard: socket() failed";
    return false;
  }
  const int one = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons(static_cast<uint16_t>(port_));
  if (bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0 ||
      listen(fd, 16) < 0) {
    LOG(ERROR) << "HTTP leaderboard: cannot bind port " << port_;
    close(fd);
    return false;
  }
  // Published before the serve thread starts and cleared only after it joins.
  listen_fd_.store(fd);
  thread_ = std::thread(&HttpLeaderboard::ServeLoop, this);
  return true;
}

auto HttpLeaderboard::bound_port() const -> int {
  const int fd = listen_fd_.load();
  if (fd < 0) {
    return -1;
  }
  sockaddr_in addr{};
  socklen_t len = sizeof(addr);
  if (getsockname(fd, reinterpret_cast<sockaddr *>(&addr), &len) < 0) {
    return -1;
  }
  return ntohs(addr.sin_port);
}

void HttpLeaderboard::Stop() {
  stop_ = true;
  const int fd = listen_fd_.load();
  if (fd >= 0) {
    // Wake accept() without closing yet: the serve thread may still be using
    // the descriptor, and a closed fd number can be handed straight back out
    // to another thread's socket().
    shutdown(fd, SHUT_RDWR);
  }
  if (thread_.joinable()) {
    thread_.join();
  }
  if (listen_fd_.exchange(-1) >= 0) {
    close(fd);
  }
}

void HttpLeaderboard::ServeLoop() {
  // Stable for the thread's whole life: Stop() joins before clearing it.
  const int listen_fd = listen_fd_.load();
  while (!stop_) {
    const int fd = accept(listen_fd, nullptr, nullptr);
    if (fd < 0) {
      if (!stop_) {
        LOG(ERROR) << "HTTP leaderboard: accept() failed";
      }
      return;
    }
    SetSocketTimeouts(fd);
    HandleConnection(fd);
    close(fd);
  }
}

void HttpLeaderboard::HandleConnection(int fd) {
  char buffer[4096];
  const ssize_t n = recv(fd, buffer, sizeof(buffer) - 1, 0);
  if (n <= 0) {
    return;
  }
  buffer[n] = '\0';
  std::string method, path;
  std::istringstream request(buffer);
  request >> method >> path;
  if (method != "GET") {
    Respond(fd, 405, "Method Not Allowed", "text/plain", "GET only\n");
    return;
  }
  if (path == "/" || path == "/index.html") {
    Respond(fd, 200, "OK", "text/html; charset=utf-8",
            RenderLeaderboardHtml());
  } else if (path == "/api/leaderboard") {
    Respond(fd, 200, "OK", "application/json", RenderLeaderboardJson());
  } else if (path == "/api/games") {
    Respond(fd, 200, "OK", "application/json", RenderGamesJson());
  } else if (path == "/api/candidates" && candidates_ != nullptr) {
    Respond(fd, 200, "OK", "application/json", RenderCandidatesJson());
  } else {
    Respond(fd, 404, "Not Found", "text/plain", "not found\n");
  }
}

auto HttpLeaderboard::RenderLeaderboardHtml() const -> std::string {
  const proto::RatingStore snapshot = elo_store_->Snapshot();
  std::vector<LeaderboardRow> rows;
  for (const auto &[key, rating] : snapshot.ratings()) {
    const auto tab = key.find('\t');
    rows.push_back(LeaderboardRow{.game = key.substr(0, tab),
                                  .player = key.substr(tab + 1),
                                  .rating = rating});
  }
  std::sort(rows.begin(), rows.end(), [](const auto &a, const auto &b) {
    if (a.game != b.game) return a.game < b.game;
    return a.rating.elo() > b.rating.elo();
  });

  std::ostringstream html;
  html << "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
          "<meta http-equiv=\"refresh\" content=\"5\">"
          "<title>Tournament Leaderboard</title>"
          "<style>body{font-family:sans-serif;margin:2em}"
          "table{border-collapse:collapse}"
          "td,th{border:1px solid #ccc;padding:4px 10px;text-align:right}"
          "th{background:#eee}td.l{text-align:left}</style></head><body>"
          "<h1>Tournament Leaderboard</h1>"
          "<table><tr><th>Rank</th><th>Player</th><th>Game</th><th>ELO</th>"
          "<th>W</th><th>D</th><th>L</th></tr>";
  std::string current_game;
  int rank = 0;
  for (const auto &row : rows) {
    if (row.game != current_game) {
      current_game = row.game;
      rank = 1;
    }
    char elo[32];
    std::snprintf(elo, sizeof(elo), "%.1f", row.rating.elo());
    html << "<tr><td>" << rank++ << "</td><td class=\"l\">"
         << HtmlEscape(row.player) << "</td><td class=\"l\">"
         << HtmlEscape(row.game) << "</td><td>" << elo << "</td><td>"
         << row.rating.wins() << "</td><td>" << row.rating.draws()
         << "</td><td>" << row.rating.losses() << "</td></tr>";
  }
  html << "</table><p><a href=\"/api/leaderboard\">JSON</a> &middot; "
          "<a href=\"/api/games\">recent games</a></p></body></html>";
  return html.str();
}

auto HttpLeaderboard::RenderCandidatesJson() const -> std::string {
  std::ostringstream json;
  json << "[";
  bool first = true;
  for (const auto &candidate : candidates_->List()) {
    if (!first) json << ",";
    first = false;
    const auto rating =
        elo_store_->Get(candidate.game(), candidate.candidate_id());
    json << "{\"candidate_id\":\"" << JsonEscape(candidate.candidate_id())
         << "\",\"display_name\":\"" << JsonEscape(candidate.display_name())
         << "\",\"author\":\"" << JsonEscape(candidate.author())
         << "\",\"game\":\"" << JsonEscape(candidate.game())
         << "\",\"parent_id\":\"" << JsonEscape(candidate.parent_id())
         << "\",\"status\":\""
         << tournament_arena::proto::Candidate::Status_Name(candidate.status())
         << "\",\"elo\":" << rating.elo() << ",\"wins\":" << rating.wins()
         << ",\"draws\":" << rating.draws()
         << ",\"losses\":" << rating.losses()
         << ",\"submitted_unix_ms\":" << candidate.submitted_unix_ms() << "}";
  }
  json << "]";
  return json.str();
}

auto HttpLeaderboard::RenderLeaderboardJson() const -> std::string {
  const proto::RatingStore snapshot = elo_store_->Snapshot();
  std::ostringstream json;
  json << "[";
  bool first = true;
  for (const auto &[key, rating] : snapshot.ratings()) {
    const auto tab = key.find('\t');
    if (!first) json << ",";
    first = false;
    json << "{\"game\":\"" << JsonEscape(key.substr(0, tab))
         << "\",\"player\":\"" << JsonEscape(key.substr(tab + 1))
         << "\",\"elo\":" << rating.elo() << ",\"wins\":" << rating.wins()
         << ",\"draws\":" << rating.draws() << ",\"losses\":"
         << rating.losses() << "}";
  }
  json << "]";
  return json.str();
}

auto HttpLeaderboard::RenderGamesJson() const -> std::string {
  std::ostringstream json;
  json << "[";
  bool first = true;
  for (const auto &line : history_->RecentGames(100)) {
    if (!first) json << ",";
    first = false;
    json << line;  // Already a JSON object.
  }
  json << "]";
  return json.str();
}

}  // namespace tournament_broker
