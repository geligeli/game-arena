#include "game_arena/server/matchmaking.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <regex>
#include <sstream>

#include "absl/log/log.h"
#include "absl/strings/numbers.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_join.h"
#include "absl/strings/str_split.h"
#include "absl/time/clock.h"
#include "absl/time/time.h"
#include "game_arena/standings/http_leaderboard.h"

namespace tournament_arena {

namespace {

using tournament_broker::HtmlEscape;
using tournament_broker::trueskill::Rating;

constexpr std::size_t kRecent = 2;     // opponents not met again at once
constexpr std::size_t kFinished = 30;  // matches /pool lists

int64_t NowMs() { return absl::ToUnixMillis(absl::Now()); }

std::string When(int64_t unix_ms) {
  return absl::FormatTime("%b %d %H:%M", absl::FromUnixMillis(unix_ms),
                          absl::LocalTimeZone());
}

// TrueSkill's match quality for two players: the chance of a draw, relative
// to the most even pairing possible.
double Quality(const Rating &a, const Rating &b, double beta) {
  const double spread = 2 * beta * beta + a.sigma * a.sigma + b.sigma * b.sigma;
  const double gap = a.mu - b.mu;
  return std::sqrt(2 * beta * beta / spread) *
         std::exp(-gap * gap / (2 * spread));
}

}  // namespace

std::vector<PoolMember> PoolOf(std::vector<PoolMember> rated, int size,
                               int newcomer_games) {
  std::stable_sort(rated.begin(), rated.end(),
                   [](const auto &a, const auto &b) {
                     return a.rating.Conservative() > b.rating.Conservative();
                   });
  std::vector<PoolMember> pool;
  for (std::size_t i = 0; i < rated.size(); ++i) {
    if (static_cast<int>(i) < size || rated[i].pool_games < newcomer_games) {
      pool.push_back(rated[i]);
    }
  }
  return pool;
}

std::optional<std::pair<std::string, std::string>> ChoosePair(
    const std::vector<PoolMember> &pool,
    const std::map<std::string, std::deque<std::string>> &recent,
    const tournament_broker::trueskill::Params &params) {
  if (pool.size() < 2) {
    return std::nullopt;
  }
  // The least certain first, less so the more it is already playing.
  const auto urgency = [](const PoolMember &m) {
    return m.rating.sigma / (1 + m.running);
  };
  const PoolMember &a = *std::ranges::max_element(
      pool,
      [&](const auto &x, const auto &y) { return urgency(x) < urgency(y); });
  const auto met = recent.find(a.id);
  const auto is_recent = [&](const PoolMember &b) {
    return met != recent.end() &&
           std::ranges::find(met->second, b.id) != met->second.end();
  };
  const bool everyone_recent = std::ranges::all_of(
      pool, [&](const PoolMember &b) { return b.id == a.id || is_recent(b); });
  const PoolMember *best = nullptr;
  double best_score = -1;
  for (const PoolMember &b : pool) {
    if (b.id == a.id || (!everyone_recent && is_recent(b))) {
      continue;
    }
    // Close and uncertain: where a game moves the ratings most.
    const double score =
        Quality(a.rating, b.rating, params.beta) *
        (a.rating.sigma * a.rating.sigma + b.rating.sigma * b.rating.sigma) /
        (1 + b.running);
    if (score > best_score) {
      best_score = score;
      best = &b;
    }
  }
  return std::pair{a.id, best->id};
}

Matchmaker::Matchmaker(proto::Matchmaking config, std::string game,
                       std::vector<std::string> builtins, Scheduler *scheduler,
                       const CandidateStore *candidates,
                       const TrueSkillStandings *ratings,
                       tournament_broker::trueskill::Params params,
                       std::filesystem::path state)
    : config_(std::move(config)),
      game_(std::move(game)),
      builtins_(std::move(builtins)),
      scheduler_(scheduler),
      candidates_(candidates),
      ratings_(ratings),
      params_(params),
      state_(std::move(state)) {
  Load();
}

Matchmaker::~Matchmaker() {
  {
    std::lock_guard lock(mutex_);
    stopping_ = true;
  }
  wake_.notify_all();
  if (thread_.joinable()) {
    thread_.join();
  }
}

void Matchmaker::Start() { thread_ = std::thread(&Matchmaker::Run, this); }

void Matchmaker::Load() {
  std::ifstream in(state_);
  std::map<int64_t, ChartSnapshot> snapshots;
  for (std::string line; std::getline(in, line);) {
    const std::vector<std::string> f = absl::StrSplit(line, '\t');
    int64_t ms = 0;
    if (f.size() < 2 || !absl::SimpleAtoi(f[1], &ms)) {
      continue;
    }
    if (f[0] == "G" && f.size() == 8) {
      Match m{.a = f[3], .b = f[4], .unix_ms = ms, .job_id = f[2]};
      if (!absl::SimpleAtoi(f[5], &m.games) ||
          !absl::SimpleAtoi(f[6], &m.a_wins) ||
          !absl::SimpleAtoi(f[7], &m.b_wins)) {
        continue;
      }
      pool_games_[m.a] += m.games;
      pool_games_[m.b] += m.games;
      finished_.push_front(m);
      if (finished_.size() > kFinished) {
        finished_.pop_back();
      }
      ++matches_done_;
    } else if (f[0] == "S" && f.size() == 5) {
      Rating r;
      if (!absl::SimpleAtod(f[3], &r.mu) || !absl::SimpleAtod(f[4], &r.sigma)) {
        continue;
      }
      ChartSnapshot &snapshot = snapshots[ms];
      snapshot.x = static_cast<double>(ms);
      snapshot.ratings[f[2]] = r;
      last_snapshot_ms_ = std::max(last_snapshot_ms_, ms);
    }
  }
  for (auto &[ms, snapshot] : snapshots) {
    history_.push_back(std::move(snapshot));
  }
  if (matches_done_ > 0) {
    LOG(INFO) << "Matchmaking: resumed after " << matches_done_ << " matches";
  }
}

void Matchmaker::Append(const std::string &line) const {
  std::ofstream(state_, std::ios::app) << line << "\n";
}

std::vector<PoolMember> Matchmaker::Rated(
    const std::map<std::string, int> &pool_games,
    const std::map<std::string, int> &running) const {
  std::vector<PoolMember> rated;
  for (const proto::Candidate &c : candidates_->List()) {
    if (c.status() != proto::Candidate::READY) {
      continue;
    }
    const auto games = pool_games.find(c.candidate_id());
    const auto playing = running.find(c.candidate_id());
    rated.push_back(
        {.id = c.candidate_id(),
         .rating = ratings_->RatingOf(c.candidate_id()),
         .pool_games = games == pool_games.end() ? 0 : games->second,
         .running = playing == running.end() ? 0 : playing->second});
  }
  return rated;
}

std::set<std::string> Matchmaker::Members() const {
  std::map<std::string, int> games;
  {
    std::lock_guard lock(mutex_);
    games = pool_games_;
  }
  std::set<std::string> ids;
  for (const PoolMember &m :
       PoolOf(Rated(games, {}), config_.pool(), config_.newcomer_games())) {
    ids.insert(m.id);
  }
  return ids;
}

void Matchmaker::Run() {
  while (true) {
    {
      std::unique_lock lock(mutex_);
      wake_.wait_for(lock, std::chrono::seconds(2), [&] { return stopping_; });
      if (stopping_) {
        return;
      }
    }
    TopUp();
    MaybeSnapshot();
  }
}

void Matchmaker::TopUp() {
  // Outside our lock: the scheduler calls OnConcluded under its own.
  if (scheduler_->queued_orders() > 0) {
    return;  // placement first; a match only fills a slot nobody wants
  }
  int free = scheduler_->total_slots() - scheduler_->in_flight_orders();
  if (free <= 0) {
    return;
  }
  std::map<std::string, int> games, running;
  std::map<std::string, std::deque<std::string>> recent;
  {
    std::lock_guard lock(mutex_);
    games = pool_games_;
    recent = recent_;
    for (const auto &[job, m] : running_) {
      ++running[m.a];
      ++running[m.b];
    }
  }
  std::vector<PoolMember> pool =
      PoolOf(Rated(games, running), config_.pool(), config_.newcomer_games());
  for (; free > 0; --free) {
    const auto pair = ChoosePair(pool, recent, params_);
    if (!pair.has_value()) {
      return;
    }
    const auto candidate = candidates_->Get(pair->first);
    if (!candidate.has_value()) {
      return;
    }
    const std::string job =
        scheduler_->EnqueueMatch(*candidate, "player:" + pair->second,
                                 static_cast<int>(config_.games()));
    for (PoolMember &m : pool) {
      m.running += m.id == pair->first || m.id == pair->second;
    }
    for (const auto &[x, y] : {*pair, std::pair{pair->second, pair->first}}) {
      std::deque<std::string> &last = recent[x];
      last.push_front(y);
      if (last.size() > kRecent) {
        last.pop_back();
      }
    }
    std::lock_guard lock(mutex_);
    recent_ = recent;
    // A match that could not even start concluded inside EnqueueMatch.
    if (!early_.erase(job)) {
      running_[job] = {.a = pair->first,
                       .b = pair->second,
                       .unix_ms = NowMs(),
                       .job_id = job};
    }
  }
}

void Matchmaker::OnConcluded(const proto::Job &job) {
  std::lock_guard lock(mutex_);
  const auto it = running_.find(job.job_id());
  if (it == running_.end()) {
    early_.insert(job.job_id());
    return;
  }
  Match m = it->second;
  running_.erase(it);
  m.unix_ms = NowMs();
  m.games = job.games_played();
  m.a_wins = job.wins();
  m.b_wins = job.losses();
  pool_games_[m.a] += m.games;
  pool_games_[m.b] += m.games;
  finished_.push_front(m);
  if (finished_.size() > kFinished) {
    finished_.pop_back();
  }
  ++matches_done_;
  Append(absl::StrJoin({std::string("G"), std::to_string(m.unix_ms), m.job_id,
                        m.a, m.b, std::to_string(m.games),
                        std::to_string(m.a_wins), std::to_string(m.b_wins)},
                       "\t"));
  wake_.notify_all();
}

void Matchmaker::MaybeSnapshot() {
  const int64_t now = NowMs();
  {
    std::lock_guard lock(mutex_);
    if (now - last_snapshot_ms_ < int64_t{config_.snapshot_s()} * 1000) {
      return;
    }
    last_snapshot_ms_ = now;
  }
  ChartSnapshot snapshot;
  snapshot.x = static_cast<double>(now);
  std::vector<std::string> ids = builtins_;
  for (const proto::Candidate &c : candidates_->List()) {
    if (c.status() == proto::Candidate::READY) {
      ids.push_back(c.candidate_id());
    }
  }
  std::string lines;
  for (const std::string &id : ids) {
    if (!ratings_->has(id)) {
      continue;  // not a game yet: nothing to draw
    }
    const Rating r = ratings_->RatingOf(id);
    snapshot.ratings[id] = r;
    absl::StrAppend(&lines, "S\t", now, "\t", id, "\t", Fixed(r.mu, 6), "\t",
                    Fixed(r.sigma, 6), "\n");
  }
  std::lock_guard lock(mutex_);
  history_.push_back(std::move(snapshot));
  std::ofstream(state_, std::ios::app) << lines;
}

std::optional<std::pair<std::string, std::string>> Matchmaker::Route(
    std::string_view target) const {
  if (target != "/pool") {
    return std::nullopt;
  }
  // Before our lock: the scheduler calls OnConcluded under its own.
  const int workers = scheduler_->worker_count();
  const int slots = scheduler_->total_slots();
  const int in_flight = scheduler_->in_flight_orders();
  const std::vector<proto::Candidate> candidates = candidates_->List();
  std::map<std::string, int> games;
  std::vector<Match> running, finished;
  std::vector<ChartSnapshot> history;
  int64_t done = 0;
  {
    std::lock_guard lock(mutex_);
    games = pool_games_;
    for (const auto &[job, m] : running_) {
      running.push_back(m);
    }
    finished.assign(finished_.begin(), finished_.end());
    history = history_;
    done = matches_done_;
  }
  const std::vector<PoolMember> pool =
      PoolOf(Rated(games, {}), config_.pool(), config_.newcomer_games());
  std::map<std::string, int> place;  // 1-based, in the pool
  for (std::size_t i = 0; i < pool.size(); ++i) {
    place[pool[i].id] = static_cast<int>(i) + 1;
  }

  // Every version that has played, and the builtins as levels.
  std::vector<ChartEntry> entries;
  ChartSnapshot now;
  now.x = static_cast<double>(NowMs());
  static const std::regex kVersion(".*-v([0-9]+)");
  std::vector<proto::Candidate> oldest_first(candidates.rbegin(),
                                             candidates.rend());
  for (const proto::Candidate &c : oldest_first) {
    const std::string &id = c.candidate_id();
    if (c.status() != proto::Candidate::READY || !ratings_->has(id)) {
      continue;
    }
    const Standing standing = ratings_->Get(id);
    std::smatch version;
    const bool in_pool = place.contains(id);
    const auto played = games.find(id);
    const int pool_games = played == games.end() ? 0 : played->second;
    ChartEntry &e = entries.emplace_back();
    e.id = id;
    e.participant = c.author().empty() ? id : c.author();
    e.version = std::regex_match(id, version, kVersion)
                    ? std::stoi(version[1].str())
                    : 1;
    e.submitted_unix_ms = c.submitted_unix_ms();
    e.rating = ratings_->RatingOf(id);
    e.wins = standing.wins;
    e.losses = standing.losses;
    e.bold = in_pool;
    e.dim = !in_pool;
    e.note = !in_pool ? "out"
             : pool_games < static_cast<int>(config_.newcomer_games())
                 ? absl::StrCat("newcomer ", pool_games, "/",
                                config_.newcomer_games())
                 : absl::StrCat("pool #", place[id]);
    now.ratings[id] = e.rating;
  }
  for (const std::string &builtin : builtins_) {
    if (ratings_->has(builtin)) {
      const Standing standing = ratings_->Get(builtin);
      ChartEntry &e = entries.emplace_back();
      e.id = builtin;
      e.rating = ratings_->RatingOf(builtin);
      e.wins = standing.wins;
      e.losses = standing.losses;
      now.ratings[builtin] = e.rating;
    }
  }
  // The charts walk a participant's versions in order.
  std::stable_sort(entries.begin(), entries.end(),
                   [](const ChartEntry &a, const ChartEntry &b) {
                     return std::pair(a.participant, a.version) <
                            std::pair(b.participant, b.version);
                   });
  history.push_back(now);
  std::vector<std::pair<double, std::string>> ticks;
  const double first = history.front().x, last = history.back().x;
  for (int i = 0; i <= 5; ++i) {
    const double t = first + (last - first) * i / 5.0;
    ticks.emplace_back(t, When(static_cast<int64_t>(t)));
  }

  std::ostringstream html;
  html << tournament_broker::PageStart("Matchmaking", /*refresh=*/true)
       << kSkillChartStyle << "<div class=viz>";
  html << "<p>Pool of " << pool.size() << " (the top " << config_.pool()
       << " by mu &minus; 3&sigma;, and newcomers until "
       << config_.newcomer_games() << " pool games) &middot; " << running.size()
       << " matches running, " << done << " done &middot; " << in_flight
       << " of " << slots << " slots busy on " << workers << " worker(s) "
       << "&middot; " << config_.games() << " games per match</p>";
  html << SkillCharts(
      entries, history, ticks,
      {.converge = "TrueSkill mu, the bar &plusmn;2&sigma;. Bold: the pool, "
                   "which keeps playing; grey: dropped out, no longer "
                   "scheduled.",
       .convergence = absl::StrCat(
           "Mu every ", config_.snapshot_s() / 60,
           " minutes; the pool drawn heavier. Versions join where they "
           "first played.")});

  const auto link = [](const std::string &job) {
    return absl::StrCat("<a href=\"/jobs/", job, "\">", job, "</a>");
  };
  html << "<h2>Running</h2><table><tr><th>a</th><th>b</th><th>since</th>"
          "<th>job</th></tr>";
  for (const Match &m : running) {
    html << "<tr><td class=l>" << HtmlEscape(m.a) << "</td><td class=l>"
         << HtmlEscape(m.b) << "</td><td>" << When(m.unix_ms)
         << "</td><td class=l>" << link(m.job_id) << "</td></tr>";
  }
  html << "</table><h2>Finished</h2><table><tr><th>a</th><th>b</th>"
          "<th>a's W-L</th><th>when</th><th>job</th></tr>";
  for (const Match &m : finished) {
    html << "<tr><td class=l>" << HtmlEscape(m.a) << "</td><td class=l>"
         << HtmlEscape(m.b) << "</td><td>" << m.a_wins << "-" << m.b_wins
         << "</td><td>" << When(m.unix_ms) << "</td><td class=l>"
         << link(m.job_id) << "</td></tr>";
  }
  html << "</table></div></body></html>";
  return std::pair{std::string("text/html; charset=utf-8"), html.str()};
}

}  // namespace tournament_arena
