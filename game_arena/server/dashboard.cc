#include "game_arena/server/dashboard.h"

#include <algorithm>
#include <array>
#include <boost/json/object.hpp>
#include <boost/json/parse.hpp>
#include <boost/json/value.hpp>
#include <cstdint>
#include <functional>
#include <map>
#include <sstream>
#include <vector>

#include "absl/strings/ascii.h"
#include "absl/strings/escaping.h"
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

// A builtin has no page.
std::string PlayerLink(std::string_view name) {
  if (!IsSafeId(name)) {
    return HtmlEscape(name);
  }
  return absl::StrCat("<a href=\"/participants/", name, "\">", HtmlEscape(name),
                      "</a>");
}

std::string PlayerLinks(const std::vector<std::string>& players) {
  return absl::StrJoin(players, " vs ", [](std::string* out, const auto& name) {
    out->append(PlayerLink(name));
  });
}

std::string OpponentLink(std::string_view spec) {
  if (spec.empty()) {
    return "graded run";
  }
  return absl::ConsumePrefix(&spec, "player:")
             ? absl::StrCat("vs ", PlayerLink(spec))
             : absl::StrCat("vs ", HtmlEscape(spec));
}

// Drops ESC [ ... letter: every example's .bazelrc asks for compiler colour.
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

// A state or action may serialize to anything; only text is shown.
std::string Readable(std::string_view bytes) {
  const bool text = std::ranges::all_of(bytes, [](char c) {
    return static_cast<unsigned char>(c) >= 0x20 || c == '\n' || c == '\t' ||
           c == '\r';
  });
  return text ? HtmlEscape(bytes)
              : absl::StrCat("(", bytes.size(), " bytes, not text)");
}

// A replay view: text that may colour itself with ANSI SGR (ESC [ n;... m),
// shown as spans. Any other escape or control byte makes it not text.
std::string ViewHtml(std::string_view bytes) {
  static constexpr std::array<std::string_view, 16> kPalette = {
      "#000", "#c33", "#3a3", "#c90", "#36c", "#a3a", "#3aa", "#ddd",
      "#666", "#f55", "#5d5", "#fd5", "#59f", "#d5d", "#5dd", "#fff"};
  std::string out;
  std::string_view fg;
  std::string_view bg;
  bool bold = false;
  bool open = false;
  std::size_t run = 0;  // start of the text not yet appended
  for (std::size_t i = 0; i < bytes.size();) {
    const auto c = static_cast<unsigned char>(bytes[i]);
    if (c >= 0x20 || c == '\n' || c == '\t' || c == '\r') {
      ++i;
      continue;
    }
    std::size_t end = i + 2;
    while (end < bytes.size() &&
           (absl::ascii_isdigit(bytes[end]) || bytes[end] == ';')) {
      ++end;
    }
    if (c != 0x1b || i + 1 >= bytes.size() || bytes[i + 1] != '[' ||
        end >= bytes.size() || bytes[end] != 'm') {
      return absl::StrCat("(", bytes.size(), " bytes, not text)");
    }
    absl::StrAppend(&out, HtmlEscape(bytes.substr(run, i - run)));
    for (std::string_view param :
         absl::StrSplit(bytes.substr(i + 2, end - i - 2), ';')) {
      int n = 0;
      if (!param.empty() && !absl::SimpleAtoi(param, &n)) {
        continue;
      }
      if (n == 0) {
        fg = bg = {};
        bold = false;
      } else if (n == 1 || n == 22) {
        bold = n == 1;
      } else if (n >= 30 && n <= 37) {
        fg = kPalette[n - 30];
      } else if (n >= 90 && n <= 97) {
        fg = kPalette[n - 90 + 8];
      } else if (n == 39) {
        fg = {};
      } else if (n >= 40 && n <= 47) {
        bg = kPalette[n - 40];
      } else if (n >= 100 && n <= 107) {
        bg = kPalette[n - 100 + 8];
      } else if (n == 49) {
        bg = {};
      }
    }
    if (open) {
      out += "</span>";
    }
    open = !fg.empty() || !bg.empty() || bold;
    if (open) {
      absl::StrAppend(
          &out, "<span style=\"", fg.empty() ? "" : "color:", fg,
          fg.empty() ? "" : ";", bg.empty() ? "" : "background:", bg,
          bg.empty() ? "" : ";", bold ? "font-weight:bold;" : "", "\">");
    }
    i = run = end + 1;
  }
  absl::StrAppend(&out, HtmlEscape(bytes.substr(run)), open ? "</span>" : "");
  return out;
}

// Any file is an asset; the extension only picks the header, which browsers
// insist on for a module script and for streamed WebAssembly.
std::string_view ContentType(std::string_view name) {
  static constexpr std::array<std::pair<std::string_view, std::string_view>, 10>
      kTypes = {{{".js", "text/javascript; charset=utf-8"},
                 {".mjs", "text/javascript; charset=utf-8"},
                 {".wasm", "application/wasm"},
                 {".svg", "image/svg+xml"},
                 {".json", "application/json"},
                 {".css", "text/css; charset=utf-8"},
                 {".html", "text/html; charset=utf-8"},
                 {".png", "image/png"},
                 {".txt", "text/plain; charset=utf-8"},
                 {".map", "application/json"}}};
  for (const auto& [extension, type] : kTypes) {
    if (name.ends_with(extension)) {
      return type;
    }
  }
  return "application/octet-stream";
}

// A JSON string that is also safe inside <script>: no "<", ">" or "&".
std::string JsonString(std::string_view text) {
  std::string out = "\"";
  for (const char c : text) {
    const auto byte = static_cast<unsigned char>(c);
    if (c == '"' || c == '\\') {
      out += '\\';
      out += c;
    } else if (byte < 0x20 || c == '<' || c == '>' || c == '&') {
      absl::StrAppend(&out, "\\u00", absl::Hex(byte, absl::kZeroPad2));
    } else {
      out += c;
    }
  }
  return out + "\"";
}

std::string FirstLine(std::string_view text) {
  const std::string_view line = text.substr(0, text.find('\n'));
  return line.size() <= 120 ? std::string(line)
                            : absl::StrCat(line.substr(0, 120), "...");
}

std::string BuildSummary(const JobRecord& record) {
  std::string summary = "-";
  for (const JobRecord::Order& order : record.orders()) {
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

std::string SourceOf(const std::string& patch) {
  Patch parsed;
  std::string error;
  if (!ParseUnifiedDiff(patch, &parsed, &error) ||
      !std::ranges::all_of(parsed.files, &PatchFile::is_new)) {
    return Pre(patch);
  }
  std::string html;
  for (const PatchFile& file : parsed.files) {
    absl::StrAppend(&html, "<h3>", HtmlEscape(file.path()), "</h3>",
                    Pre(file.added_content));
  }
  return html;
}

std::string JobRow(const JobRecord& record, bool show_source) {
  const proto::Job& job = record.job();
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

std::string Field(const json::object& game, std::string_view key) {
  const json::value* value = game.if_contains(key);
  return value != nullptr && value->is_string()
             ? std::string(value->as_string())
             : std::string();
}

int64_t Number(const json::object& game, std::string_view key) {
  const json::value* value = game.if_contains(key);
  return value != nullptr && value->is_int64() ? value->as_int64() : 0;
}

std::vector<std::string> Players(const json::object& game) {
  std::vector<std::string> players;
  for (int seat = 0; game.contains("player" + std::to_string(seat)); ++seat) {
    players.push_back(Field(game, "player" + std::to_string(seat)));
  }
  return players;
}

std::string ResultText(int64_t result, int64_t winner,
                       const std::vector<std::string>& players) {
  if (result == GameRecord::DRAW) {
    return "draw";
  }
  if (result == GameRecord::WIN && winner >= 0 &&
      winner < static_cast<int64_t>(players.size())) {
    return PlayerLink(players[winner]) + " won";
  }
  return "-";
}

// A frame is one step; it shows its own view, or the last one before it.
// The URL's #<frame> is the one shown, so a move can be linked to.
constexpr std::string_view kReplayScript = R"(<script>
var frames=document.querySelectorAll('.f'),views=document.querySelectorAll('.v'),
at=0,timer=null;
function go(i){at=Math.max(0,Math.min(frames.length-1,i));
frames.forEach(function(f,j){f.hidden=j!=at});
var v=+frames[at].dataset.v;views.forEach(function(e,k){e.hidden=k!=v});
if(window.showView)showView(at,v);
document.getElementById('slider').value=at;history.replaceState(null,'','#'+at)}
function seek(d){for(var i=at+d;i>=0&&i<frames.length;i+=d){
if(frames[i].dataset.own){go(i);return}}go(d<0?0:frames.length-1)}
function play(){if(timer){clearTimeout(timer);timer=null;return}
(function tick(){timer=setTimeout(function(){if(at>=frames.length-1){timer=null;
return}go(at+1);tick()},+document.getElementById('speed').value)})()}
document.onkeydown=function(e){var k={ArrowLeft:function(){go(at-1)},
ArrowRight:function(){go(at+1)},ArrowUp:function(){seek(-1)},
ArrowDown:function(){seek(1)},' ':play,Home:function(){go(0)},
End:function(){go(frames.length-1)}}[e.key];if(k){e.preventDefault();k()}};
go(+location.hash.slice(1)||0);
</script>)";

// With a replay module: it draws each view (bytes, base64 in #views) into
// #stage, and init sees them all, for whatever spans the game; a module that
// fails says so instead.
constexpr std::string_view kModuleScript = R"(<script>
(function(){var stage=document.getElementById('stage'),
raw=JSON.parse(document.getElementById('views').textContent),
game=JSON.parse(document.getElementById('game').textContent),cache={};
function bytes(v){if(!(v in cache)){var s=atob(raw[v]),b=new Uint8Array(s.length);
for(var i=0;i<s.length;i++)b[i]=s.charCodeAt(i);cache[v]=b}return cache[v]}
import(game.module).then(function(m){
return Promise.resolve(m.init&&m.init(stage,game,raw.map(function(_,v){
return bytes(v)}))).then(function(){
window.showView=function(i,v){var f=frames[i];m.render(stage,bytes(v),
{index:i,view:v,player:+f.dataset.p,caption:f.textContent})};
showView(at,+frames[at].dataset.v)})}).catch(function(e){
stage.textContent='(replay module failed to load: '+e+')'})})();
</script>)";

}  // namespace

Dashboard::Dashboard(const CandidateStore* candidates, const JobLog* jobs,
                     const tournament_broker::GameHistory* games,
                     const Standings* standings, bool show_source,
                     ReplayAssets assets)
    : candidates_(candidates),
      jobs_(jobs),
      games_(games),
      standings_(standings),
      show_source_(show_source),
      assets_(std::move(assets)) {}

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

  // A lookup, never a path on disk: only the problem's own files are here.
  if (absl::ConsumePrefix(&path, "/assets/")) {
    const auto asset = assets_.files.find(path);
    if (asset == assets_.files.end()) {
      return std::nullopt;
    }
    return std::pair(std::string(ContentType(asset->first)), asset->second);
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
  for (const JobRecord& record : jobs_->List()) {
    html << JobRow(record, show_source_);
  }
  html << "</table>" << kPageEnd;
  return html.str();
}

std::optional<std::string> Dashboard::JobPage(const std::string& job_id) const {
  const std::optional<JobRecord> record = jobs_->Get(job_id);
  if (!record.has_value()) {
    return std::nullopt;
  }
  const proto::Job& job = record->job();
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

  for (const JobRecord::Order& order : record->orders()) {
    html << "<h2>" << OpponentLink(order.opponent_spec()) << "</h2>";
    if (!order.has_result()) {
      html << "<p>Not back yet.</p>";
      continue;
    }
    const proto::OrderResult& result = order.result();
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
    for (const auto& [target, digest] : result.artifacts()) {
      html << " &middot; archived " << HtmlEscape(target) << " as "
           << HtmlEscape(digest.substr(0, 12));
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
    const std::string& output = result.build_output().empty()
                                    ? result.build_log()
                                    : result.build_output();
    if (!output.empty()) {
      html << "<details" << (result.build_ok() ? "" : " open")
           << "><summary>Build output</summary>" << Pre(output) << "</details>";
    }
  }

  html << "<h2>Submission</h2>";
  const std::optional<proto::Candidate> candidate =
      candidates_->Get(job.candidate_id());
  if (!show_source_) {
    html << kHidden;
  } else if (!record->submission().patch().empty()) {
    html << SourceOf(record->submission().patch());
  } else if (candidate.has_value()) {
    // A match the coordinator starts records no patch: a version never changes.
    html << CodeOf(*candidate);
  }
  html << kPageEnd;
  return html.str();
}

std::optional<std::string> Dashboard::ParticipantPage(
    const std::string& id) const {
  const std::optional<proto::Candidate> candidate = candidates_->Get(id);
  // A participant's name stands for its newest version, as in arena_cli.
  if (!candidate.has_value() && candidates_->versions()) {
    if (const auto latest = candidates_->Latest(id)) {
      return ParticipantPage(latest->candidate_id());
    }
  }
  std::vector<JobRecord> submissions;
  for (JobRecord& record : jobs_->List()) {
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
  for (const JobRecord& record : submissions) {
    html << JobRow(record, show_source_);
  }
  html << "</table>";

  if (candidate.has_value()) {
    html << "<h2>Current code</h2><p>"
         << proto::Candidate::Status_Name(candidate->status()) << ", submitted "
         << Time(candidate->submitted_unix_ms()) << "</p>";
    html << (show_source_ ? CodeOf(*candidate) : std::string(kHidden));
  }
  html << kPageEnd;
  return html.str();
}

std::string Dashboard::CodeOf(const proto::Candidate& candidate) const {
  std::string html;
  if (!candidate.build_error().empty()) {
    html += Pre(candidate.build_error());
  }
  for (const std::string& path : candidate.file_paths()) {
    std::string error;
    const std::optional<std::string> source =
        candidates_->ReadSource(candidate.candidate_id(), path, &error);
    absl::StrAppend(&html, "<h3>", HtmlEscape(path), "</h3>",
                    Pre(source.value_or(error)));
  }
  return html;
}

std::string Dashboard::GamesPage(int page, const std::string& player) const {
  std::vector<json::object> games;
  for (const std::string& line : games_->AllGames()) {
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
  // The index is in arrival order, and an order sends its games at the end.
  std::ranges::stable_sort(games, std::greater{}, [](const json::object& game) {
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
    const json::object& game = games[i];
    const std::vector<std::string> players = Players(game);
    html << "<tr><td class=\"l\">" << Time(Number(game, "finished_unix_ms"))
         << "</td><td class=\"l\">" << PlayerLinks(players)
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
    const std::string& game_id) const {
  const std::optional<GameRecord> record = games_->Load(game_id);
  if (!record.has_value()) {
    return std::nullopt;
  }
  const std::vector<std::string> players(record->player_names().begin(),
                                         record->player_names().end());

  std::ostringstream html;
  html << PageStart(record->game() + ": " + absl::StrJoin(players, " vs "))
       << "<p>" << PlayerLinks(players) << " &middot; "
       << ResultText(record->result(), record->winning_player(), players)
       << " (" << HtmlEscape(record->termination_reason()) << ") &middot; "
       << record->steps_size() << " moves &middot; "
       << Time(record->started_unix_ms()) << " to "
       << Time(record->finished_unix_ms()) << "</p>"
       << "<p><button onclick=\"go(0)\">first</button> "
          "<button onclick=\"seek(-1)\">prev view</button> "
          "<button onclick=\"go(at-1)\">back</button> "
          "<button onclick=\"play()\">play</button> "
          "<button onclick=\"go(at+1)\">forward</button> "
          "<button onclick=\"seek(1)\">next view</button> "
          "<button onclick=\"go(frames.length-1)\">last</button> "
          "<select id=\"speed\"><option value=\"100\">0.1 s</option>"
          "<option value=\"250\">0.25 s</option>"
          "<option value=\"400\" selected>0.4 s</option>"
          "<option value=\"1000\">1 s</option>"
          "<option value=\"2000\">2 s</option></select> "
          "<input id=\"slider\" type=\"range\" min=\"0\" max=\""
       << record->steps_size()
       << "\" value=\"0\" oninput=\"go(+this.value)\"> "
          "<small>&larr;&rarr; step, &uarr;&darr; view, space play</small></p>";

  // Each view once; a frame points at its own or the latest before it, so a
  // step that leaves the view unchanged costs no bytes here either.
  std::vector<std::string> views;
  // A game recorded before views existed shows its state instead.
  views.push_back(record->initial_view().empty()
                      ? Readable(record->initial_state())
                      : ViewHtml(record->initial_view()));
  // The same views as bytes, for a replay module.
  std::vector<std::string> raw = {record->initial_view().empty()
                                      ? record->initial_state()
                                      : record->initial_view()};
  std::ostringstream frames;
  frames << "<div class=\"f\" data-v=\"0\" data-own=\"1\"><p>Start</p></div>";
  for (int i = 0; i < record->steps_size(); ++i) {
    const GameRecord::Step& step = record->steps(i);
    const bool own = !step.view().empty();
    if (own) {
      views.push_back(ViewHtml(step.view()));
      raw.push_back(step.view());
    }
    frames << "<div class=\"f\" data-v=\"" << views.size() - 1 << "\" data-p=\""
           << step.player() << "\"" << (own ? " data-own=\"1\"" : "")
           << "><p>Move " << i + 1 << ": ";
    if (step.player() >= 0 &&
        step.player() < static_cast<int>(players.size())) {
      frames << "seat " << step.player() << " ("
             << HtmlEscape(players[step.player()]) << ")";
    } else {
      frames << "chance";
    }
    if (!step.caption().empty()) {
      frames << ": " << ViewHtml(step.caption()) << "</p></div>";
    } else {
      frames << (step.player() >= 0 ? " played" : ":") << " <code>"
             << Readable(step.action()) << "</code></p></div>";
    }
  }
  if (!assets_.module.empty()) {
    std::string encoded = "[";
    for (const std::string& view : raw) {
      absl::StrAppend(&encoded, encoded.size() > 1 ? "," : "", "\"",
                      absl::Base64Escape(view), "\"");
    }
    std::string game = absl::StrCat(
        "{\"game\":", JsonString(record->game()),
        ",\"module\":", JsonString(absl::StrCat("/assets/", assets_.module)),
        ",\"players\":[");
    for (std::size_t seat = 0; seat < players.size(); ++seat) {
      absl::StrAppend(&game, seat > 0 ? "," : "", JsonString(players[seat]));
    }
    html << frames.str() << "<div id=\"stage\"></div>"
         << "<script type=\"application/json\" id=\"views\">" << encoded
         << "]</script><script type=\"application/json\" id=\"game\">" << game
         << "]}</script>" << kReplayScript << kModuleScript << kPageEnd;
    return html.str();
  }

  // The tallest view holds the controls in place while stepping.
  std::size_t lines = 1;
  for (const std::string& view : views) {
    lines = std::max(
        lines, static_cast<std::size_t>(std::ranges::count(view, '\n')) + 1);
  }
  html << frames.str() << "<div style=\"min-height:" << lines * 1.25 + 1
       << "em\">";
  for (std::size_t k = 0; k < views.size(); ++k) {
    html << "<pre class=\"v\" hidden>" << views[k] << "</pre>";
  }
  html << "</div>";
  html << kReplayScript << kPageEnd;
  return html.str();
}

}  // namespace tournament_arena
