#include "game_arena/server/matchmaking.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <numeric>
#include <regex>
#include <set>
#include <sstream>
#include <tuple>

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

}  // namespace

std::vector<PoolMember> PoolOf(std::vector<PoolMember> rated, int size) {
  const auto low = [](const PoolMember &m) {
    return m.rating.mu - 2 * m.rating.sigma;
  };
  const auto high = [](const PoolMember &m) {
    return m.rating.mu + 2 * m.rating.sigma;
  };
  std::ranges::stable_sort(rated, std::greater{}, low);
  const std::size_t top = std::min<std::size_t>(size, rated.size());
  if (top == 0) {
    return {};
  }
  // The bar to plausibly make the top, and the best any outsider could do.
  const double bar = low(rated[top - 1]);
  double challenger = -INFINITY;
  for (std::size_t i = top; i < rated.size(); ++i) {
    challenger = std::max(challenger, high(rated[i]));
  }
  std::vector<PoolMember> pool;
  for (std::size_t i = 0; i < rated.size(); ++i) {
    if (i < top || high(rated[i]) >= bar) {
      rated[i].settled = i < top && low(rated[i]) > challenger;
      pool.push_back(rated[i]);
    }
  }
  return pool;
}

std::optional<std::vector<std::string>> ChooseGroup(
    const std::vector<PoolMember> &pool,
    const std::map<std::string, std::deque<std::string>> &recent,
    const tournament_broker::trueskill::Params &params, std::size_t seats,
    const std::vector<PoolMember> &fillers) {
  if (pool.size() < 2 || pool.size() + fillers.size() < seats) {
    return std::nullopt;
  }
  // The least certain first, less so the more it is already playing; while
  // anyone's place is open, one of those.
  const bool open = !std::ranges::all_of(pool, &PoolMember::settled);
  const auto urgency = [open](const PoolMember &m) {
    return open && m.settled ? -1 : m.rating.sigma / (1 + m.running);
  };
  const PoolMember &a = *std::ranges::max_element(
      pool,
      [&](const auto &x, const auto &y) { return urgency(x) < urgency(y); });
  const auto met = recent.find(a.id);
  const auto is_recent = [&](const PoolMember &b) {
    return met != recent.end() &&
           std::ranges::find(met->second, b.id) != met->second.end();
  };
  // Too few members for a game: every one plays, and fillers take the rest.
  const bool whole = pool.size() < seats;
  std::vector<const PoolMember *> fixed, rivals;
  for (const PoolMember &m : pool) {
    if (m.id != a.id) {
      (whole ? fixed : rivals).push_back(&m);
    }
  }
  if (whole) {
    for (const PoolMember &f : fillers) {
      rivals.push_back(&f);
    }
  }
  // Every seats - 1 - |fixed| of the rivals, by index.
  const std::size_t k = seats - 1 - fixed.size();
  std::vector<std::size_t> pick(k);
  std::iota(pick.begin(), pick.end(), std::size_t{0});
  std::optional<std::tuple<bool, bool, double>> best_key;
  std::vector<std::string> best;
  for (;;) {
    std::vector<Rating> ratings = {a.rating};
    std::set<std::string> authors = {a.author};
    bool fresh = true;
    double variance = a.rating.sigma * a.rating.sigma;
    int running = 0;
    std::vector<const PoolMember *> group = fixed;
    for (const std::size_t i : pick) {
      group.push_back(rivals[i]);
    }
    for (const PoolMember *member : group) {
      const PoolMember &b = *member;
      ratings.push_back(b.rating);
      authors.insert(b.author);
      fresh = fresh && !is_recent(b);
      variance += b.rating.sigma * b.rating.sigma;
      running += b.running;
    }
    // Close and uncertain: where a game moves the ratings most.
    const auto key =
        std::tuple(seats == 2 || authors.size() == seats, fresh,
                   tournament_broker::trueskill::Quality(ratings, params.beta) *
                       variance / (1 + running));
    if (!best_key.has_value() || key > *best_key) {
      best_key = key;
      best = {a.id};
      for (const PoolMember *member : group) {
        best.push_back(member->id);
      }
    }
    std::size_t i = k;
    while (i > 0 && pick[i - 1] == rivals.size() - k + i - 1) {
      --i;
    }
    if (i == 0) {
      return best;
    }
    ++pick[i - 1];
    for (std::size_t j = i; j < k; ++j) {
      pick[j] = pick[j - 1] + 1;
    }
  }
}

Matchmaker::Matchmaker(proto::Matchmaking config, std::string game, int seats,
                       std::vector<std::string> builtins, Scheduler *scheduler,
                       const CandidateStore *candidates,
                       const TrueSkillStandings *ratings,
                       tournament_broker::trueskill::Params params,
                       std::filesystem::path state)
    : config_(std::move(config)),
      game_(std::move(game)),
      seats_(seats),
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
  for (std::string line; std::getline(in, line);) {
    const std::vector<std::string> f = absl::StrSplit(line, '\t');
    int64_t ms = 0;
    if (f.size() < 2 || !absl::SimpleAtoi(f[1], &ms) || f[0] != "G") {
      continue;
    }
    Match m{.unix_ms = ms, .job_id = f[2]};
    // Before groups: "G ms job a b games a_wins b_wins".
    const bool pair = f.size() == 8;
    if (pair) {
      m.members = {f[3], f[4]};
    } else if (f.size() == 6) {
      m.members = absl::StrSplit(f[4], ',');
    } else {
      continue;
    }
    const std::vector<std::string> finishes =
        pair ? std::vector{f[6], f[7]}
             : std::vector<std::string>(
                   absl::StrSplit(f[5], ',', absl::SkipEmpty()));
    bool ok = absl::SimpleAtoi(f[pair ? 5 : 3], &m.games);
    for (const std::string &count : finishes) {
      ok = ok && absl::SimpleAtoi(count, &m.finishes.emplace_back());
    }
    if (ok) {
      finished_.push_front(m);
      if (finished_.size() > kFinished) {
        finished_.pop_back();
      }
      ++matches_done_;
    }
  }
  if (matches_done_ > 0) {
    LOG(INFO) << "Matchmaking: resumed after " << matches_done_ << " matches";
  }
}

void Matchmaker::Append(const std::string &line) const {
  std::ofstream(state_, std::ios::app) << line << "\n";
}

std::vector<PoolMember> Matchmaker::Rated(
    const std::map<std::string, int> &running) const {
  std::vector<PoolMember> rated;
  for (const proto::Candidate &c : candidates_->List()) {
    if (c.status() != proto::Candidate::READY) {
      continue;
    }
    const auto playing = running.find(c.candidate_id());
    rated.push_back(
        {.id = c.candidate_id(),
         .author = c.author().empty() ? c.candidate_id() : c.author(),
         .rating = ratings_->RatingOf(c.candidate_id()),
         .running = playing == running.end() ? 0 : playing->second});
  }
  return rated;
}

std::set<std::string> Matchmaker::Members() const {
  std::set<std::string> ids;
  for (const PoolMember &m : PoolOf(Rated({}), config_.pool())) {
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
  std::map<std::string, int> running;
  std::map<std::string, std::deque<std::string>> recent;
  {
    std::lock_guard lock(mutex_);
    recent = recent_;
    for (const auto &[job, m] : running_) {
      for (const std::string &member : m.members) {
        ++running[member];
      }
    }
  }
  std::vector<PoolMember> pool = PoolOf(Rated(running), config_.pool());
  std::vector<PoolMember> fillers;
  for (const std::string &builtin : builtins_) {
    fillers.push_back({.id = builtin,
                       .author = builtin,
                       .rating = ratings_->RatingOf(builtin),
                       .running = running[builtin]});
  }
  for (; free > 0; --free) {
    const auto group = ChooseGroup(pool, recent, params_, seats_, fillers);
    if (!group.has_value()) {
      return;
    }
    const auto candidate = candidates_->Get(group->front());
    if (!candidate.has_value()) {
      return;
    }
    std::vector<std::string> opponents;
    for (auto it = group->begin() + 1; it != group->end(); ++it) {
      opponents.push_back(it->starts_with("builtin:") ? *it : "player:" + *it);
    }
    const std::string job = scheduler_->EnqueueMatch(
        *candidate, opponents, static_cast<int>(config_.games()));
    for (std::vector<PoolMember> *members : {&pool, &fillers}) {
      for (PoolMember &m : *members) {
        m.running += std::ranges::contains(*group, m.id);
      }
    }
    for (const std::string &x : *group) {
      for (const std::string &y : *group) {
        if (x == y) {
          continue;
        }
        std::deque<std::string> &last = recent[x];
        last.push_front(y);
        if (last.size() > kRecent * (seats_ - 1)) {
          last.pop_back();
        }
      }
    }
    std::lock_guard lock(mutex_);
    recent_ = recent;
    // A match that could not even start concluded inside EnqueueMatch.
    if (!early_.erase(job)) {
      running_[job] = {.members = *group, .unix_ms = NowMs(), .job_id = job};
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
  m.finishes.assign(job.finishes().begin(), job.finishes().end());
  finished_.push_front(m);
  if (finished_.size() > kFinished) {
    finished_.pop_back();
  }
  ++matches_done_;
  Append(absl::StrJoin({std::string("G"), std::to_string(m.unix_ms), m.job_id,
                        std::to_string(m.games), absl::StrJoin(m.members, ","),
                        absl::StrJoin(m.finishes, ",")},
                       "\t"));
  wake_.notify_all();
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
  std::vector<Match> running, finished;
  int64_t done = 0;
  {
    std::lock_guard lock(mutex_);
    for (const auto &[job, m] : running_) {
      running.push_back(m);
    }
    finished.assign(finished_.begin(), finished_.end());
    done = matches_done_;
  }
  const std::vector<PoolMember> pool = PoolOf(Rated({}), config_.pool());
  std::map<std::string, const PoolMember *> member;
  std::map<std::string, int> place;  // 1-based, in the pool
  for (std::size_t i = 0; i < pool.size(); ++i) {
    member[pool[i].id] = &pool[i];
    place[pool[i].id] = static_cast<int>(i) + 1;
  }

  // Every version that has played, and the builtins as levels.
  std::vector<ChartEntry> entries;
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
                      : absl::StrCat("pool #", place[id],
                                     member[id]->settled ? "" : ", place open");
  }
  for (const std::string &builtin : builtins_) {
    if (ratings_->has(builtin)) {
      const Standing standing = ratings_->Get(builtin);
      ChartEntry &e = entries.emplace_back();
      e.id = builtin;
      e.rating = ratings_->RatingOf(builtin);
      e.wins = standing.wins;
      e.losses = standing.losses;
    }
  }
  // The charts walk a participant's versions in order.
  std::stable_sort(entries.begin(), entries.end(),
                   [](const ChartEntry &a, const ChartEntry &b) {
                     return std::pair(a.participant, a.version) <
                            std::pair(b.participant, b.version);
                   });
  std::ostringstream html;
  html << tournament_broker::PageStart("Matchmaking", /*refresh=*/true)
       << kSkillChartStyle << "<div class=viz>";
  html << "<p>Pool of " << pool.size() << " (the top " << config_.pool()
       << ": every version whose mu + 2&sigma; reaches the " << config_.pool()
       << "th best mu &minus; 2&sigma;) &middot; " << running.size()
       << " matches running, " << done << " done &middot; " << in_flight
       << " of " << slots << " slots busy on " << workers << " worker(s) "
       << "&middot; " << config_.games() << " games per match</p>";
  html << SkillCharts(entries,
                      "TrueSkill mu, the bar &plusmn;2&sigma;. Bold: the pool, "
                      "which keeps playing; grey: dropped out, no longer "
                      "scheduled.");

  const auto link = [](const std::string &job) {
    return absl::StrCat("<a href=\"/jobs/", job, "\">", job, "</a>");
  };
  html << "<h2>Running</h2><table><tr><th>match</th><th>since</th>"
          "<th>job</th></tr>";
  for (const Match &m : running) {
    html << "<tr><td class=l>" << HtmlEscape(absl::StrJoin(m.members, " vs "))
         << "</td><td>" << When(m.unix_ms) << "</td><td class=l>"
         << link(m.job_id) << "</td></tr>";
  }
  html << "</table><h2>Finished</h2><table><tr><th>match</th>"
          "<th>first's places, 1st-2nd-&hellip;</th><th>when</th>"
          "<th>job</th></tr>";
  for (const Match &m : finished) {
    html << "<tr><td class=l>" << HtmlEscape(absl::StrJoin(m.members, " vs "))
         << "</td><td>" << absl::StrJoin(m.finishes, "-") << "</td><td>"
         << When(m.unix_ms) << "</td><td class=l>" << link(m.job_id)
         << "</td></tr>";
  }
  html << "</table></div></body></html>";
  return std::pair{std::string("text/html; charset=utf-8"), html.str()};
}

}  // namespace tournament_arena
