#include "game_arena/server/dashboard.h"

#include <algorithm>
#include <boost/json/object.hpp>
#include <boost/json/parse.hpp>
#include <boost/json/value.hpp>
#include <cstdint>
#include <functional>
#include <map>
#include <sstream>
#include <vector>

#include "absl/strings/ascii.h"
#include "absl/strings/numbers.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_join.h"
#include "absl/strings/str_split.h"
#include "absl/strings/strip.h"
#include "absl/time/time.h"
#include "game_arena/server/unified_diff.h"
#include "game_arena/standings/http_leaderboard.h"

namespace tournament_arena {

namespace {

namespace json = boost::json;
using tournament_broker::HtmlEscape;
using tournament_broker::IsSafeId;
using tournament_broker::PageStart;
using GameRecord = tournament_broker::proto::GameRecord;

constexpr std::string_view kHtml = "text/html; charset=utf-8";
constexpr std::size_t kGamesPerPage = 100;
constexpr std::string_view kHidden =
    "<p><i>Hidden by this problem's source policy.</i></p>";
constexpr std::string_view kPageEnd = "</body></html>";

std::string Time(int64_t unix_ms) {
  if (unix_ms == 0) {
    return "-";
  }
  return absl::FormatTime("%Y-%m-%d %H:%M:%S", absl::FromUnixMillis(unix_ms),
                          absl::UTCTimeZone());
}

// A player, linked to their page. A builtin has none.
std::string PlayerLink(std::string_view name) {
  if (!IsSafeId(name)) {
    return HtmlEscape(name);
  }
  return absl::StrCat("<a href=\"/participants/", name, "\">", HtmlEscape(name),
                      "</a>");
}

std::string OpponentLink(std::string_view spec) {
  if (spec.empty()) {
    return "graded run";
  }
  return absl::ConsumePrefix(&spec, "player:")
             ? absl::StrCat("vs ", PlayerLink(spec))
             : absl::StrCat("vs ", HtmlEscape(spec));
}

// Without terminal colour codes (ESC [ ... letter): every example's .bazelrc
// asks the compiler for colour.
std::string StripAnsi(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (std::size_t i = 0; i < text.size(); ++i) {
    if (text[i] == '\x1b' && i + 1 < text.size() && text[i + 1] == '[') {
      for (i += 2; i < text.size() && !absl::ascii_isalpha(text[i]); ++i) {
      }
      continue;
    }
    out += text[i];
  }
  return out;
}

std::string Pre(std::string_view text) {
  return absl::StrCat("<pre>", HtmlEscape(StripAnsi(text)), "</pre>");
}

// A game's bytes as a person can read them: a state or action may serialize
// to anything, and only text is worth showing.
std::string Readable(std::string_view bytes) {
  const bool text = std::ranges::all_of(bytes, [](char c) {
    return static_cast<unsigned char>(c) >= 0x20 || c == '\n' || c == '\t' ||
           c == '\r';
  });
  return text ? HtmlEscape(bytes)
              : absl::StrCat("(", bytes.size(), " bytes, not text)");
}

// Enough of an error to tell jobs apart in a table.
std::string FirstLine(std::string_view text) {
  const std::string_view line = text.substr(0, text.find('\n'));
  return line.size() <= 120 ? std::string(line)
                            : absl::StrCat(line.substr(0, 120), "...");
}

// "ok" when every order that came back built, "failed" when one did not.
std::string BuildSummary(const JobRecord &record) {
  std::string summary = "-";
  for (const JobRecord::Order &order : record.orders()) {
    if (!order.has_result()) {
      continue;
    }
    if (!order.result().build_ok()) {
      return "failed";
    }
    summary = "ok";
  }
  return summary;
}

// A submission's files, rebuilt from its patch; a patch that changes files
// already in the tree is shown as it is.
std::string SourceOf(const std::string &patch) {
  Patch parsed;
  std::string error;
  if (!ParseUnifiedDiff(patch, &parsed, &error) ||
      !std::ranges::all_of(parsed.files, &PatchFile::is_new)) {
    return Pre(patch);
  }
  std::string html;
  for (const PatchFile &file : parsed.files) {
    absl::StrAppend(&html, "<h3>", HtmlEscape(file.path()), "</h3>",
                    Pre(file.added_content));
  }
  return html;
}

std::string JobRow(const JobRecord &record, bool show_source) {
  const proto::Job &job = record.job();
  return absl::StrCat(
      "<tr><td class=\"l\">", Time(job.created_unix_ms()),
      "</td><td class=\"l\"><a href=\"/jobs/", HtmlEscape(job.job_id()), "\">",
      HtmlEscape(job.job_id()), "</a></td><td class=\"l\">",
      PlayerLink(job.candidate_id()), "</td><td class=\"l\">",
      proto::Job::State_Name(job.state()), "</td><td class=\"l\">",
      BuildSummary(record), "</td><td>", job.wins(), "</td><td>", job.draws(),
      "</td><td>", job.losses(), "</td><td class=\"l\">",
      show_source ? HtmlEscape(StripAnsi(FirstLine(job.error()))) : "",
      "</td></tr>");
}

constexpr std::string_view kJobHeader =
    "<table><tr><th>Submitted</th><th>Job</th><th>Participant</th>"
    "<th>State</th><th>Build</th><th>W</th><th>D</th><th>L</th>"
    "<th>Error</th></tr>";

// One line of the games index, as the dashboard reads it.
std::string Field(const json::object &game, std::string_view key) {
  const json::value *value = game.if_contains(key);
  return value != nullptr && value->is_string()
             ? std::string(value->as_string())
             : std::string();
}

int64_t Number(const json::object &game, std::string_view key) {
  const json::value *value = game.if_contains(key);
  return value != nullptr && value->is_int64() ? value->as_int64() : 0;
}

std::vector<std::string> Players(const json::object &game) {
  std::vector<std::string> players;
  for (int seat = 0; game.contains("player" + std::to_string(seat)); ++seat) {
    players.push_back(Field(game, "player" + std::to_string(seat)));
  }
  return players;
}

std::string ResultText(int64_t result, int64_t winner,
                       const std::vector<std::string> &players) {
  if (result == GameRecord::DRAW) {
    return "draw";
  }
  if (result == GameRecord::WIN && winner >= 0 &&
      winner < static_cast<int64_t>(players.size())) {
    return PlayerLink(players[winner]) + " won";
  }
  return "-";
}

// Steps through the frames above it: first, back, play, forward, last, a
// slider, and the arrow keys.
constexpr std::string_view kReplayScript = R"(<script>
var frames=document.querySelectorAll('.f'),at=0,timer=null;
function go(i){at=Math.max(0,Math.min(frames.length-1,i));
frames.forEach(function(f,j){f.hidden=j!=at});
document.getElementById('slider').value=at}
function play(){if(timer){clearInterval(timer);timer=null;return}
timer=setInterval(function(){if(at>=frames.length-1){clearInterval(timer);
timer=null}else{go(at+1)}},400)}
document.onkeydown=function(e){if(e.key=='ArrowLeft')go(at-1);
if(e.key=='ArrowRight')go(at+1)};
go(0);
</script>)";

}  // namespace

Dashboard::Dashboard(const CandidateStore *candidates, const JobLog *jobs,
                     const tournament_broker::GameHistory *games,
                     const Standings *standings, bool show_source)
    : candidates_(candidates),
      jobs_(jobs),
      games_(games),
      standings_(standings),
      show_source_(show_source) {}

std::optional<std::pair<std::string, std::string>> Dashboard::Route(
    std::string_view target) const {
  // Nothing is percent-decoded: every id linked here is [A-Za-z0-9_-].
  std::string_view path = target.substr(0, target.find('?'));
  std::map<std::string, std::string, std::less<>> params;
  if (path.size() < target.size()) {
    for (std::string_view pair :
         absl::StrSplit(target.substr(path.size() + 1), '&')) {
      const std::size_t eq = pair.find('=');
      params[std::string(pair.substr(0, eq))] =
          eq == std::string_view::npos ? "" : pair.substr(eq + 1);
    }
  }

  std::optional<std::string> body;
  if (path == "/jobs") {
    body = JobsPage();
  } else if (absl::ConsumePrefix(&path, "/jobs/")) {
    body = JobPage(std::string(path));
  } else if (absl::ConsumePrefix(&path, "/participants/")) {
    body = ParticipantPage(std::string(path));
  } else if (path == "/games") {
    int page = 0;
    if (!absl::SimpleAtoi(params["page"], &page) || page < 0) {
      page = 0;
    }
    body = GamesPage(page, params["player"]);
  } else if (absl::ConsumePrefix(&path, "/games/")) {
    body = ReplayPage(std::string(path));
  }
  if (!body.has_value()) {
    return std::nullopt;
  }
  return std::pair(std::string(kHtml), std::move(*body));
}

std::string Dashboard::JobsPage() const {
  std::ostringstream html;
  html << PageStart("Jobs") << kJobHeader;
  for (const JobRecord &record : jobs_->List()) {
    html << JobRow(record, show_source_);
  }
  html << "</table>" << kPageEnd;
  return html.str();
}

std::optional<std::string> Dashboard::JobPage(const std::string &job_id) const {
  const std::optional<JobRecord> record = jobs_->Get(job_id);
  if (!record.has_value()) {
    return std::nullopt;
  }
  const proto::Job &job = record->job();
  std::ostringstream html;
  html << PageStart("Job " + job_id) << "<p>" << PlayerLink(job.candidate_id())
       << " &middot; submitted "
       << Time(record->submission().submitted_unix_ms()) << " &middot; "
       << proto::Job::State_Name(job.state()) << " &middot; W/D/L "
       << job.wins() << "/" << job.draws() << "/" << job.losses()
       << " &middot; finished " << Time(job.finished_unix_ms()) << "</p>";
  if (!job.error().empty()) {
    html << (show_source_ ? Pre(job.error()) : std::string(kHidden));
  }

  for (const JobRecord::Order &order : record->orders()) {
    html << "<h2>" << OpponentLink(order.opponent_spec()) << "</h2>";
    if (!order.has_result()) {
      html << "<p>Not back yet.</p>";
      continue;
    }
    const proto::OrderResult &result = order.result();
    html << "<p>" << (result.build_ok() ? "built" : "build failed") << " on "
         << HtmlEscape(result.worker_id().empty() ? "-" : result.worker_id());
    if (!result.machine_class().empty()) {
      html << " (" << HtmlEscape(result.machine_class()) << ")";
    }
    html << " &middot; W/D/L " << result.wins() << "/" << result.draws() << "/"
         << result.losses();
    for (int i = 0; i < order.game_ids_size(); ++i) {
      html << (i == 0 ? " &middot; games " : " ") << "<a href=\"/games/"
           << HtmlEscape(order.game_ids(i)) << "\">" << i + 1 << "</a>";
    }
    html << "</p>";
    if (!show_source_) {
      html << kHidden;
      continue;
    }
    if (!result.error().empty()) {
      html << Pre(result.error());
    }
    // An older worker sends only the compacted diagnostics of a failure.
    const std::string &output = result.build_output().empty()
                                    ? result.build_log()
                                    : result.build_output();
    if (!output.empty()) {
      html << "<details" << (result.build_ok() ? "" : " open")
           << "><summary>Build output</summary>" << Pre(output) << "</details>";
    }
  }

  html << "<h2>Submission</h2>"
       << (show_source_ ? SourceOf(record->submission().patch())
                        : std::string(kHidden))
       << kPageEnd;
  return html.str();
}

std::optional<std::string> Dashboard::ParticipantPage(
    const std::string &id) const {
  const std::optional<proto::Candidate> candidate = candidates_->Get(id);
  std::vector<JobRecord> submissions;
  for (JobRecord &record : jobs_->List()) {
    if (record.job().candidate_id() == id) {
      submissions.push_back(std::move(record));
    }
  }
  if (!candidate.has_value() && submissions.empty()) {
    return std::nullopt;
  }

  std::ostringstream html;
  html << PageStart(candidate.has_value() ? candidate->display_name() : id)
       << "<p><a href=\"/games?player=" << HtmlEscape(id) << "\">Games</a>";
  if (standings_ != nullptr && standings_->has(id)) {
    const Standing row = standings_->Get(id);
    html << " &middot; " << HtmlEscape(standings_->score_label()) << " "
         << absl::StrCat(row.score) << " &middot; W/D/L " << row.wins << "/"
         << row.draws << "/" << row.losses;
  }
  html << "</p><h2>Submissions</h2>" << kJobHeader;
  for (const JobRecord &record : submissions) {
    html << JobRow(record, show_source_);
  }
  html << "</table>";

  if (candidate.has_value()) {
    html << "<h2>Current code</h2><p>"
         << proto::Candidate::Status_Name(candidate->status()) << ", submitted "
         << Time(candidate->submitted_unix_ms()) << "</p>";
    if (!show_source_) {
      html << kHidden;
    } else {
      if (!candidate->build_error().empty()) {
        html << Pre(candidate->build_error());
      }
      for (const std::string &path : candidate->file_paths()) {
        std::string error;
        const std::optional<std::string> source =
            candidates_->ReadSource(id, path, &error);
        html << "<h3>" << HtmlEscape(path) << "</h3>"
             << Pre(source.value_or(error));
      }
    }
  }
  html << kPageEnd;
  return html.str();
}

std::string Dashboard::GamesPage(int page, const std::string &player) const {
  std::vector<json::object> games;
  for (const std::string &line : games_->AllGames()) {
    boost::system::error_code ec;
    json::value value = json::parse(line, ec);
    if (ec || !value.is_object()) {
      continue;
    }
    const std::vector<std::string> players = Players(value.as_object());
    if (!player.empty() &&
        std::ranges::find(players, player) == players.end()) {
      continue;
    }
    games.push_back(std::move(value.as_object()));
  }
  // The index is in the order games arrived, and an order sends all of its
  // games when it finishes.
  std::ranges::stable_sort(games, std::greater{}, [](const json::object &game) {
    return Number(game, "finished_unix_ms");
  });

  std::ostringstream html;
  html << PageStart(player.empty() ? "Games" : "Games of " + player) << "<p>"
       << games.size()
       << " game(s)</p><table><tr><th>Finished</th><th>Players</th>"
          "<th>Result</th><th>Reason</th><th>Moves</th><th></th></tr>";
  const std::size_t first = static_cast<std::size_t>(page) * kGamesPerPage;
  for (std::size_t i = first; i < games.size() && i < first + kGamesPerPage;
       ++i) {
    const json::object &game = games[i];
    const std::vector<std::string> players = Players(game);
    std::vector<std::string> links;
    for (const std::string &name : players) {
      links.push_back(PlayerLink(name));
    }
    html << "<tr><td class=\"l\">" << Time(Number(game, "finished_unix_ms"))
         << "</td><td class=\"l\">" << absl::StrJoin(links, " vs ")
         << "</td><td class=\"l\">"
         << ResultText(Number(game, "result"), Number(game, "winning_player"),
                       players)
         << "</td><td class=\"l\">" << HtmlEscape(Field(game, "reason"))
         << "</td><td>" << Number(game, "moves")
         << "</td><td class=\"l\"><a href=\"/games/"
         << HtmlEscape(Field(game, "game_id")) << "\">replay</a></td></tr>";
  }
  html << "</table><p>";
  const std::string filter = player.empty() ? "" : "&player=" + player;
  if (page > 0) {
    html << "<a href=\"/games?page=" << page - 1 << HtmlEscape(filter)
         << "\">newer</a> ";
  }
  if (first + kGamesPerPage < games.size()) {
    html << "<a href=\"/games?page=" << page + 1 << HtmlEscape(filter)
         << "\">older</a>";
  }
  html << "</p>" << kPageEnd;
  return html.str();
}

std::optional<std::string> Dashboard::ReplayPage(
    const std::string &game_id) const {
  const std::optional<GameRecord> record = games_->Load(game_id);
  if (!record.has_value()) {
    return std::nullopt;
  }
  const std::vector<std::string> players(record->player_names().begin(),
                                         record->player_names().end());
  std::vector<std::string> links;
  for (const std::string &name : players) {
    links.push_back(PlayerLink(name));
  }

  std::ostringstream html;
  html << PageStart(record->game() + ": " + absl::StrJoin(players, " vs "))
       << "<p>" << absl::StrJoin(links, " vs ") << " &middot; "
       << ResultText(record->result(), record->winning_player(), players)
       << " (" << HtmlEscape(record->termination_reason()) << ") &middot; "
       << record->steps_size() << " moves &middot; "
       << Time(record->started_unix_ms()) << " to "
       << Time(record->finished_unix_ms()) << "</p>"
       << "<p><button onclick=\"go(0)\">first</button> "
          "<button onclick=\"go(at-1)\">back</button> "
          "<button onclick=\"play()\">play</button> "
          "<button onclick=\"go(at+1)\">forward</button> "
          "<button onclick=\"go(frames.length-1)\">last</button> "
          "<input id=\"slider\" type=\"range\" min=\"0\" max=\""
       << record->steps_size()
       << "\" value=\"0\" oninput=\"go(+this.value)\"></p>";

  // A game recorded before views existed shows its state instead.
  html << "<div class=\"f\"><p>Start</p><pre>"
       << Readable(record->initial_view().empty() ? record->initial_state()
                                                  : record->initial_view())
       << "</pre></div>";
  for (int i = 0; i < record->steps_size(); ++i) {
    const GameRecord::Step &step = record->steps(i);
    html << "<div class=\"f\"><p>Move " << i + 1 << ": ";
    if (step.player() >= 0 &&
        step.player() < static_cast<int>(players.size())) {
      html << "seat " << step.player() << " ("
           << HtmlEscape(players[step.player()]) << ") played";
    } else {
      html << "chance:";
    }
    html << " <code>" << Readable(step.action()) << "</code></p>";
    if (!step.view().empty()) {
      html << "<pre>" << Readable(step.view()) << "</pre>";
    }
    html << "</div>";
  }
  html << kReplayScript << kPageEnd;
  return html.str();
}

}  // namespace tournament_arena
