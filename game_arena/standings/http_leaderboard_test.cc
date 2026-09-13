#include "game_arena/standings/http_leaderboard.h"

#include <boost/asio/connect.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/json/parse.hpp>
#include <boost/json/value.hpp>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace tournament_broker {
namespace {

namespace beast = boost::beast;
namespace http = beast::http;
namespace json = boost::json;
namespace net = boost::asio;
using tcp = net::ip::tcp;

using ::testing::HasSubstr;
using ::testing::Not;
using tournament_arena::Standing;

// Two rows whose display name and author carry characters the HTML and JSON
// escapers each have to handle.
class FakeStandings : public tournament_arena::Standings {
 public:
  void Record(const std::string &, const std::string &,
              const tournament_arena::proto::OrderResult &) override {}

  Standing Get(const std::string &candidate_id) const override {
    for (const Standing &row : Rank(0)) {
      if (row.candidate_id == candidate_id) return row;
    }
    return Standing{};
  }

  std::vector<Standing> Rank(int) const override {
    Standing first;
    first.candidate_id = "cand-1";
    first.score = 1512.5;
    first.wins = 3;
    first.draws = 2;
    first.losses = 1;
    Standing second;
    second.candidate_id = "cand-2";
    second.score = 1488.0;
    return {first, second};
  }

  std::string score_label() const override { return "elo"; }
  bool has(const std::string &) const override { return true; }
};

class FakeCandidates : public tournament_arena::CandidateView {
 public:
  std::vector<tournament_arena::proto::Candidate> List() const override {
    return {Make()};
  }

  std::optional<tournament_arena::proto::Candidate> Get(
      const std::string &candidate_id) const override {
    if (candidate_id != "cand-1") return std::nullopt;
    return Make();
  }

 private:
  static tournament_arena::proto::Candidate Make() {
    tournament_arena::proto::Candidate candidate;
    candidate.set_candidate_id("cand-1");
    // Both escapers get something to do: the angle brackets and ampersand are
    // HTML, the quote and backslash are JSON.
    candidate.set_display_name("<b>Ada</b> & \"Co\"");
    candidate.set_author("path\\to\\author");
    return candidate;
  }
};

// A blocking GET against the loopback listener, so the test drives the server
// over a real socket rather than calling its renderers directly.
http::response<http::string_body> Get(int port, const std::string &target,
                                      http::verb method = http::verb::get) {
  net::io_context ioc;
  beast::tcp_stream stream(ioc);
  stream.connect(
      {net::ip::make_address("127.0.0.1"), static_cast<unsigned short>(port)});
  http::request<http::string_body> request(method, target, 11);
  request.set(http::field::host, "127.0.0.1");
  http::write(stream, request);

  beast::flat_buffer buffer;
  http::response<http::string_body> response;
  http::read(stream, buffer, response);
  beast::error_code ignored;
  stream.socket().shutdown(tcp::socket::shutdown_both, ignored);
  return response;
}

class HttpLeaderboardTest : public ::testing::Test {
 protected:
  void SetUp() override {
    dir_ = std::filesystem::temp_directory_path() /
           ("http_leaderboard_test_" +
            std::to_string(::testing::UnitTest::GetInstance()->random_seed()));
    std::filesystem::create_directories(dir_);
    history_ = std::make_unique<GameHistory>(dir_);
  }

  void TearDown() override { std::filesystem::remove_all(dir_); }

  std::filesystem::path dir_;
  std::unique_ptr<GameHistory> history_;
  FakeStandings standings_;
  FakeCandidates candidates_;
};

TEST_F(HttpLeaderboardTest, ServesHtmlLeaderboardWithEscapedNames) {
  HttpLeaderboard leaderboard(0, history_.get(), &candidates_, &standings_,
                              "nim & friends");
  ASSERT_TRUE(leaderboard.Start());
  const int port = leaderboard.bound_port();
  ASSERT_GT(port, 0);

  const auto response = Get(port, "/");
  EXPECT_EQ(response.result(), http::status::ok);
  EXPECT_THAT(std::string(response[http::field::content_type]),
              HasSubstr("text/html"));
  // The problem name and the display name reach the page escaped, and no raw
  // tag from the display name survives.
  EXPECT_THAT(response.body(), HasSubstr("nim &amp; friends"));
  EXPECT_THAT(response.body(),
              HasSubstr("&lt;b&gt;Ada&lt;/b&gt; &amp; &quot;Co&quot;"));
  EXPECT_THAT(response.body(), Not(HasSubstr("<b>Ada</b>")));
  // Beast frames the response; the body length must match what it advertised.
  EXPECT_EQ(std::stoul(std::string(response[http::field::content_length])),
            response.body().size());
}

TEST_F(HttpLeaderboardTest, ServesJsonLeaderboardWithEscapedStrings) {
  HttpLeaderboard leaderboard(0, history_.get(), &candidates_, &standings_);
  ASSERT_TRUE(leaderboard.Start());

  const auto response = Get(leaderboard.bound_port(), "/api/leaderboard");
  EXPECT_EQ(response.result(), http::status::ok);
  EXPECT_EQ(std::string(response[http::field::content_type]),
            "application/json");

  // Parsed, not string-matched: the point of the assertion is that the quotes
  // and backslashes in the name survive a round trip as data rather than
  // breaking the document.
  boost::system::error_code ec;
  const json::value parsed = json::parse(response.body(), ec);
  ASSERT_FALSE(ec) << ec.message() << ": " << response.body();
  const json::object &root = parsed.as_object();
  EXPECT_EQ(root.at("score_label").as_string(), "elo");

  const json::array &rows = root.at("rows").as_array();
  ASSERT_EQ(rows.size(), 2u);
  const json::object &first = rows[0].as_object();
  EXPECT_EQ(first.at("rank").as_int64(), 1);
  EXPECT_EQ(first.at("candidate_id").as_string(), "cand-1");
  EXPECT_EQ(first.at("display_name").as_string(), "<b>Ada</b> & \"Co\"");
  EXPECT_EQ(first.at("author").as_string(), "path\\to\\author");
  EXPECT_EQ(first.at("score").as_double(), 1512.5);
  EXPECT_EQ(first.at("wins").as_int64(), 3);
  EXPECT_TRUE(first.at("metrics").as_object().empty());

  // No candidate record for this one, so the id stands in as the display name.
  const json::object &second = rows[1].as_object();
  EXPECT_EQ(second.at("rank").as_int64(), 2);
  EXPECT_EQ(second.at("display_name").as_string(), "cand-2");
  EXPECT_EQ(second.at("author").as_string(), "-");
}

TEST_F(HttpLeaderboardTest, ServesCandidatesJson) {
  HttpLeaderboard leaderboard(0, history_.get(), &candidates_, &standings_);
  ASSERT_TRUE(leaderboard.Start());

  const auto response = Get(leaderboard.bound_port(), "/api/candidates");
  EXPECT_EQ(response.result(), http::status::ok);

  boost::system::error_code ec;
  const json::value parsed = json::parse(response.body(), ec);
  ASSERT_FALSE(ec) << ec.message() << ": " << response.body();
  const json::array &candidates = parsed.as_array();
  ASSERT_EQ(candidates.size(), 1u);
  const json::object &candidate = candidates[0].as_object();
  EXPECT_EQ(candidate.at("candidate_id").as_string(), "cand-1");
  EXPECT_EQ(candidate.at("display_name").as_string(), "<b>Ada</b> & \"Co\"");
  EXPECT_EQ(candidate.at("author").as_string(), "path\\to\\author");
  // Pulled from the standings by candidate id, not from the candidate record.
  EXPECT_EQ(candidate.at("score").as_double(), 1512.5);
  EXPECT_EQ(candidate.at("losses").as_int64(), 1);
}

TEST_F(HttpLeaderboardTest, GamesIndexIsAnArrayWithoutAnArena) {
  // A standalone broker: no candidate store and no standings.
  HttpLeaderboard leaderboard(0, history_.get());
  ASSERT_TRUE(leaderboard.Start());
  const int port = leaderboard.bound_port();

  const auto games = Get(port, "/api/games");
  EXPECT_EQ(games.result(), http::status::ok);
  EXPECT_EQ(games.body(), "[]");

  // The two arena-backed routes are 404 rather than an empty table.
  EXPECT_EQ(Get(port, "/").result(), http::status::not_found);
  EXPECT_EQ(Get(port, "/api/leaderboard").result(), http::status::not_found);
  EXPECT_EQ(Get(port, "/api/candidates").result(), http::status::not_found);
}

TEST_F(HttpLeaderboardTest, GamesIndexDropsOnlyTheUnparseableLine) {
  // GameHistory seeds its in-memory tail from the index at construction, so
  // writing the file first is how a damaged line gets in front of the server.
  {
    std::ofstream index(dir_ / "index.jsonl");
    index << R"({"game_id":"g1","moves":7})" << '\n';
    index << R"({"game_id":"g2",  truncated)" << '\n';
    index << R"({"game_id":"g3","moves":9})" << '\n';
  }
  history_ = std::make_unique<GameHistory>(dir_);

  HttpLeaderboard leaderboard(0, history_.get());
  ASSERT_TRUE(leaderboard.Start());

  const auto response = Get(leaderboard.bound_port(), "/api/games");
  EXPECT_EQ(response.result(), http::status::ok);

  // The whole document stays valid; only the damaged record is missing.
  boost::system::error_code ec;
  const json::value parsed = json::parse(response.body(), ec);
  ASSERT_FALSE(ec) << ec.message() << ": " << response.body();
  const json::array &games = parsed.as_array();
  ASSERT_EQ(games.size(), 2u);
  EXPECT_EQ(games[0].as_object().at("game_id").as_string(), "g1");
  EXPECT_EQ(games[0].as_object().at("moves").as_int64(), 7);
  EXPECT_EQ(games[1].as_object().at("game_id").as_string(), "g3");
}

TEST_F(HttpLeaderboardTest, RejectsNonGetAndUnknownPaths) {
  HttpLeaderboard leaderboard(0, history_.get(), &candidates_, &standings_);
  ASSERT_TRUE(leaderboard.Start());
  const int port = leaderboard.bound_port();

  EXPECT_EQ(Get(port, "/", http::verb::post).result(),
            http::status::method_not_allowed);
  EXPECT_EQ(Get(port, "/nope").result(), http::status::not_found);
}

TEST_F(HttpLeaderboardTest, ServesConnectionsInSuccessionAndStopsCleanly) {
  HttpLeaderboard leaderboard(0, history_.get(), &candidates_, &standings_);
  ASSERT_TRUE(leaderboard.Start());
  const int port = leaderboard.bound_port();

  // Each request is its own connection; the serve coroutine has to come back
  // round to accept after every one of them.
  for (int i = 0; i < 5; ++i) {
    EXPECT_EQ(Get(port, "/api/leaderboard").result(), http::status::ok);
  }

  leaderboard.Stop();
  leaderboard.Stop();  // Idempotent: the destructor calls it again.
}

TEST_F(HttpLeaderboardTest, FailsToStartOnAPortAlreadyBound) {
  HttpLeaderboard first(0, history_.get(), &candidates_, &standings_);
  ASSERT_TRUE(first.Start());

  HttpLeaderboard second(first.bound_port(), history_.get(), &candidates_,
                         &standings_);
  EXPECT_FALSE(second.Start());
  EXPECT_EQ(second.bound_port(), -1);
}

}  // namespace
}  // namespace tournament_broker
