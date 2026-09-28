#include "game_arena/server/swiss.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <functional>
#include <random>
#include <sstream>

#include "absl/log/log.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_replace.h"
#include "absl/strings/str_split.h"
#include "absl/time/time.h"
#include "game_arena/standings/http_leaderboard.h"

namespace tournament_arena {

namespace {

using tournament_broker::HtmlEscape;
using tournament_broker::trueskill::Rating;

constexpr std::string_view kBuiltinPrefix = "builtin:";

std::pair<std::string, std::string> Key(const std::string &a,
                                        const std::string &b) {
  return a < b ? std::pair{a, b} : std::pair{b, a};
}

bool IsBuiltin(const std::string &id) { return id.starts_with(kBuiltinPrefix); }

// The reference categorical order (light), by participant; builtins are grey.
constexpr const char *kSeries[] = {"#2a78d6", "#eb6834", "#1baf7a", "#eda100",
                                   "#e87ba4", "#008300", "#4a3aa7", "#e34948"};
constexpr const char *kBuiltinInk = "#898781";

std::string Fixed(double value, int digits = 1) {
  char text[32];
  std::snprintf(text, sizeof(text), "%.*f", digits, value);
  return text;
}

// A linear map from [lo, hi] onto [a, b].
struct Scale {
  double lo, hi, a, b;
  double operator()(double v) const {
    return hi == lo ? a : a + (v - lo) * (b - a) / (hi - lo);
  }
};

// Gridlines and labels every |step| along y, for a chart |width| wide.
void YAxis(const Scale &y, double step, double left, double width,
           std::ostringstream &svg) {
  for (double v = std::ceil(y.lo / step) * step; v <= y.hi; v += step) {
    svg << "<line class=grid x1=" << left << " x2=" << left + width
        << " y1=" << y(v) << " y2=" << y(v) << " />"
        << "<text class=tick x=" << left - 6 << " y=" << y(v) + 4
        << " text-anchor=end>" << Fixed(v, 0) << "</text>";
  }
}

constexpr std::string_view kStyle = R"(<style>
.viz{--surface:#fcfcfb;--ink:#0b0b0b;--ink2:#52514e;--muted:#898781;
--grid:#e1e0d9;--axis:#c3c2b7;background:var(--surface);color:var(--ink);
font-family:system-ui,-apple-system,"Segoe UI",sans-serif;max-width:900px}
.viz svg{display:block;width:100%;height:auto;overflow:visible}
.viz .grid{stroke:var(--grid);stroke-width:1}
.viz .tick{fill:var(--muted);font-size:11px;font-variant-numeric:tabular-nums}
.viz .lbl{fill:var(--ink2);font-size:11px}
.viz .live{fill:var(--ink);font-weight:600}
.viz h2{font-size:16px;margin:28px 0 4px}
.viz p.note{color:var(--ink2);font-size:13px;margin:0 0 8px}
.legend{display:flex;gap:16px;font-size:12px;color:var(--ink2);margin:4px 0}
.legend i{display:inline-block;width:10px;height:10px;border-radius:5px;
margin-right:5px;vertical-align:-1px}
.viz circle,.viz path.hit{cursor:default}
td.l{text-align:left}
</style>)";

}  // namespace

SwissRound SwissPairs(
    const std::vector<std::string> &ranked,
    const std::set<std::pair<std::string, std::string>> &played,
    const std::set<std::string> &had_bye) {
  SwissRound round;
  std::vector<std::string> pool = ranked;
  if (pool.size() % 2 == 1) {
    auto sits = std::find_if(pool.rbegin(), pool.rend(), [&](const auto &id) {
      return !had_bye.contains(id);
    });
    if (sits == pool.rend()) {
      sits = pool.rbegin();
    }
    round.bye = *sits;
    pool.erase(std::next(sits).base());
  }
  std::vector<bool> used(pool.size(), false);
  for (std::size_t i = 0; i < pool.size(); ++i) {
    if (used[i]) {
      continue;
    }
    used[i] = true;
    std::size_t pick = pool.size();
    for (std::size_t j = i + 1; j < pool.size(); ++j) {
      if (used[j]) {
        continue;
      }
      if (pick == pool.size()) {
        pick = j;  // the rematch to fall back on
      }
      if (!played.contains(Key(pool[i], pool[j]))) {
        pick = j;
        break;
      }
    }
    used[pick] = true;
    round.pairs.emplace_back(pool[i], pool[pick]);
  }
  return round;
}

std::vector<SwissEntry> SeedVersions(const JobLog &jobs,
                                     const std::string &submit_dir,
                                     const Standings &board,
                                     CandidateStore *store) {
  std::vector<JobRecord> records = jobs.List();
  std::reverse(records.begin(), records.end());  // oldest first

  std::vector<SwissEntry> entries;
  std::map<std::string, std::set<std::string>> seen;  // patches by participant
  for (const JobRecord &record : records) {
    const bool played =
        std::any_of(record.orders().begin(), record.orders().end(),
                    [](const JobRecord::Order &order) {
                      return !order.game_ids().empty();
                    });
    const proto::Candidate &submission = record.submission();
    const std::string &participant = submission.candidate_id();
    if (!played || !seen[participant].insert(submission.patch()).second) {
      continue;
    }
    char id[64];
    std::snprintf(id, sizeof(id), "%s-v%02zu", participant.c_str(),
                  seen[participant].size());

    proto::SubmitRequest request;
    request.set_author(id);
    request.set_display_name(id);
    request.set_game(submission.game());
    request.set_patch(absl::StrReplaceAll(
        submission.patch(), {{absl::StrCat(submit_dir, "/", participant, "/"),
                              absl::StrCat(submit_dir, "/", id, "/")}}));
    request.set_entry_header(submission.entry_header());
    *request.mutable_extra_deps() = submission.extra_deps();
    *request.mutable_params() = submission.params();
    std::string error;
    // Already there when a run resumes on its own data dir.
    if (!store->Get(id).has_value() &&
        !store->Create(request, &error).has_value()) {
      LOG(WARNING) << "Swiss: skipping " << id << " (job "
                   << record.job().job_id() << "): " << error;
      continue;
    }
    store->SetStatus(id, proto::Candidate::READY, "");
    SwissEntry &entry = entries.emplace_back();
    entry.id = id;
    entry.participant = participant;
    entry.version = static_cast<int>(seen[participant].size());
    entry.submitted_unix_ms = submission.submitted_unix_ms();
  }

  // Each participant's last version is the one the board ranks.
  std::map<std::string, SwissEntry *> live;
  for (SwissEntry &entry : entries) {
    live[entry.participant] = &entry;
  }
  std::vector<SwissEntry *> by_board;
  for (auto &[participant, entry] : live) {
    entry->live = true;
    by_board.push_back(entry);
  }
  std::sort(by_board.begin(), by_board.end(), [&](auto *a, auto *b) {
    return board.Get(a->participant).score > board.Get(b->participant).score;
  });
  for (std::size_t i = 0; i < by_board.size(); ++i) {
    by_board[i]->board_rank = static_cast<int>(i) + 1;
  }
  return entries;
}

SwissRun::SwissRun(std::vector<SwissEntry> entries, int rounds, int games,
                   std::string game, Scheduler *scheduler,
                   const CandidateView *candidates,
                   const TrueSkillStandings *ratings, const JobLog *jobs,
                   std::filesystem::path state)
    : entries_(std::move(entries)),
      rounds_(rounds > 0 ? rounds
                         : static_cast<int>(std::ceil(std::log2(
                               std::max<std::size_t>(2, entries_.size())))) +
                               3),
      games_(games),
      game_(std::move(game)),
      scheduler_(scheduler),
      candidates_(candidates),
      ratings_(ratings),
      jobs_(jobs),
      state_(std::move(state)) {}

SwissRun::~SwissRun() {
  {
    std::lock_guard lock(mutex_);
    stopping_ = true;
  }
  cv_.notify_all();
  if (thread_.joinable()) {
    thread_.join();
  }
}

void SwissRun::Start() { thread_ = std::thread(&SwissRun::Run, this); }

void SwissRun::OnConcluded(const proto::Job &job) {
  {
    std::lock_guard lock(mutex_);
    concluded_[job.job_id()] = job;
  }
  cv_.notify_all();
}

std::map<std::string, Rating> SwissRun::Snapshot() const {
  std::map<std::string, Rating> ratings;
  for (const SwissEntry &entry : entries_) {
    ratings[entry.id] = ratings_->RatingOf(entry.id);
  }
  return ratings;
}

void SwissRun::Append(int round,
                      const std::map<std::string, Rating> &snapshot) const {
  std::ofstream out(state_, std::ios::app);
  for (const auto &[id, rating] : snapshot) {
    out << "S\t" << round << "\t" << id << "\t" << Fixed(rating.mu, 6) << "\t"
        << Fixed(rating.sigma, 6) << "\n";
  }
}

// State lines, tab-separated: "M <round> <a> <b> <job id>" as a match is
// queued, "B <round> <id>" for a bye, and "S <round> <id> <mu> <sigma>" after
// each round, round 0 being before any game.
void SwissRun::Resume(std::set<std::pair<std::string, std::string>> *played,
                      std::set<std::string> *had_bye) {
  std::ifstream in(state_);
  for (std::string line; std::getline(in, line);) {
    const std::vector<std::string> f = absl::StrSplit(line, '\t');
    const std::size_t round = std::stoul(f[1]);
    if (f[0] == "M") {
      played_.resize(std::max(played_.size(), round));
      played_[round - 1].push_back({f[2], f[3], f[4]});
      played->insert(Key(f[2], f[3]));
      const auto record = jobs_->Get(f[4]);
      proto::Job job = record.has_value() ? record->job() : proto::Job();
      if (job.state() != proto::Job::DONE &&
          job.state() != proto::Job::FAILED) {
        job.set_job_id(f[4]);
        job.set_state(proto::Job::CANCELLED);
        job.set_error("dropped: the run stopped before it finished");
      }
      concluded_[f[4]] = job;
    } else if (f[0] == "B") {
      byes_.resize(std::max(byes_.size(), round));
      byes_[round - 1] = f[2];
      had_bye->insert(f[2]);
    } else {
      snapshots_.resize(std::max(snapshots_.size(), round + 1));
      snapshots_[round][f[2]] = {std::stod(f[3]), std::stod(f[4])};
    }
  }
  byes_.resize(played_.size());
  // A round cut short counts as played, with the games it got.
  while (snapshots_.size() < played_.size() + 1) {
    snapshots_.push_back(Snapshot());
    Append(static_cast<int>(snapshots_.size()) - 1, snapshots_.back());
  }
}

void SwissRun::Run() {
  std::vector<std::string> ids;
  for (const SwissEntry &entry : entries_) {
    ids.push_back(entry.id);
  }
  std::set<std::pair<std::string, std::string>> played;
  std::set<std::string> had_bye;
  {
    std::lock_guard lock(mutex_);
    Resume(&played, &had_bye);
  }
  if (played_.empty()) {
    // No prior: the first round is drawn, reproducibly.
    std::shuffle(ids.begin(), ids.end(), std::mt19937(1));
  }
  if (!played_.empty()) {
    LOG(INFO) << "Swiss: resuming after round " << played_.size();
  }

  for (int round = static_cast<int>(played_.size()); round < rounds_; ++round) {
    if (round > 0) {
      const auto ratings = Snapshot();
      std::stable_sort(ids.begin(), ids.end(),
                       [&](const auto &a, const auto &b) {
                         return ratings.at(a).mu > ratings.at(b).mu;
                       });
    }
    const SwissRound pairing = SwissPairs(ids, played, had_bye);
    std::vector<Match> matches;
    for (auto [a, b] : pairing.pairs) {
      played.insert(Key(a, b));
      // The candidate's side is the one with code, when only one has.
      if (IsBuiltin(a) && !IsBuiltin(b)) {
        std::swap(a, b);
      }
      proto::Candidate candidate;
      if (IsBuiltin(a)) {
        candidate.set_candidate_id(a);
        candidate.set_game(game_);
      } else {
        candidate = *candidates_->Get(a);
      }
      const std::string opponent = IsBuiltin(b) ? b : "player:" + b;
      matches.push_back(
          {a, b, scheduler_->EnqueueMatch(candidate, opponent, games_)});
      std::ofstream(state_, std::ios::app)
          << "M\t" << round + 1 << "\t" << a << "\t" << b << "\t"
          << matches.back().job_id << "\n";
    }
    if (!pairing.bye.empty()) {
      had_bye.insert(pairing.bye);
      std::ofstream(state_, std::ios::app)
          << "B\t" << round + 1 << "\t" << pairing.bye << "\n";
    }
    LOG(INFO) << "Swiss: round " << round + 1 << " of " << rounds_ << ", "
              << matches.size() << " match(es)";

    std::unique_lock lock(mutex_);
    played_.push_back(matches);
    byes_.push_back(pairing.bye);
    cv_.wait(lock, [&] {
      return stopping_ ||
             std::all_of(matches.begin(), matches.end(), [&](const Match &m) {
               return concluded_.contains(m.job_id);
             });
    });
    if (stopping_) {
      return;
    }
    snapshots_.push_back(Snapshot());
    Append(round + 1, snapshots_.back());
  }
  LOG(INFO) << "Swiss: all " << rounds_ << " rounds played";
}

std::optional<std::pair<std::string, std::string>> SwissRun::Route(
    std::string_view target) const {
  if (target != "/swiss") {
    return std::nullopt;
  }
  // Before our lock: the scheduler calls OnConcluded under its own.
  const int workers = scheduler_->worker_count();
  const int queued = scheduler_->queued_orders();
  const int running = scheduler_->in_flight_orders();
  const std::map<std::string, Rating> now = Snapshot();

  std::lock_guard lock(mutex_);
  std::map<std::string, int> slot;
  for (const SwissEntry &entry : entries_) {
    if (!entry.participant.empty() && !slot.contains(entry.participant)) {
      const int next = static_cast<int>(slot.size());
      slot[entry.participant] = next;
    }
  }
  const auto ink = [&](const SwissEntry &entry) -> std::string {
    return entry.participant.empty()
               ? kBuiltinInk
               : kSeries[slot.at(entry.participant) % std::size(kSeries)];
  };
  std::vector<const SwissEntry *> by_mu;
  for (const SwissEntry &entry : entries_) {
    by_mu.push_back(&entry);
  }
  std::sort(by_mu.begin(), by_mu.end(), [&](auto *a, auto *b) {
    return now.at(a->id).mu > now.at(b->id).mu;
  });
  double lo = 1e9, hi = -1e9;
  for (const auto &snapshot : snapshots_) {
    for (const auto &[id, r] : snapshot) {
      lo = std::min(lo, r.mu - 2 * r.sigma);
      hi = std::max(hi, r.mu + 2 * r.sigma);
    }
  }
  for (const auto &[id, r] : now) {
    lo = std::min(lo, r.mu - 2 * r.sigma);
    hi = std::max(hi, r.mu + 2 * r.sigma);
  }
  lo = std::floor(lo / 5) * 5;
  hi = std::ceil(hi / 5) * 5;

  std::ostringstream html;
  html << tournament_broker::PageStart("Swiss re-rank", /*refresh=*/true)
       << kStyle << "<div class=viz>";
  int done = 0, total = 0;
  for (const auto &round : played_) {
    for (const Match &m : round) {
      ++total;
      done += concluded_.contains(m.job_id) ? 1 : 0;
    }
  }
  const bool finished =
      snapshots_.size() == static_cast<std::size_t>(rounds_) + 1;
  html << "<p>" << (finished ? "Finished: " : "Round ")
       << (finished ? rounds_ : static_cast<int>(played_.size())) << " of "
       << rounds_ << " &middot; " << entries_.size() << " entries &middot; "
       << done << " of " << total << " matches done (" << running
       << " running, " << queued << " queued) &middot; " << workers
       << " worker(s) &middot; " << games_ << " games per match</p>";

  html << "<div class=legend>";
  for (const auto &[participant, index] : slot) {
    html << "<span><i style=\"background:"
         << kSeries[index % std::size(kSeries)] << "\"></i>"
         << HtmlEscape(participant) << "</span>";
  }
  html << "<span><i style=\"background:" << kBuiltinInk
       << "\"></i>builtin</span></div>";

  // 1. Where everyone stands now: mu with a 2-sigma interval.
  {
    const double left = 230, width = 520, row = 16, top = 20;
    const Scale x{lo, hi, left, left + width};
    std::ostringstream svg;
    for (double v = lo; v <= hi; v += 5) {
      svg << "<line class=grid x1=" << x(v) << " x2=" << x(v)
          << " y1=" << top - 6 << " y2=" << top + row * by_mu.size()
          << " /><text class=tick x=" << x(v) << " y=" << top - 10
          << " text-anchor=middle>" << Fixed(v, 0) << "</text>";
    }
    for (std::size_t i = 0; i < by_mu.size(); ++i) {
      const SwissEntry &e = *by_mu[i];
      const Rating &r = now.at(e.id);
      const double y = top + row * i + row / 2;
      const tournament_arena::Standing s = ratings_->Get(e.id);
      const std::string tip = absl::StrCat(
          e.id, ": mu ", Fixed(r.mu), " sigma ", Fixed(r.sigma), ", ", s.wins,
          "-", s.losses,
          e.live ? absl::StrCat(", live, #", e.board_rank, " on the board")
                 : "");
      svg << "<text class=\"lbl" << (e.live ? " live" : "")
          << "\" x=" << left - 8 << " y=" << y + 4 << " text-anchor=end>"
          << i + 1 << ". " << HtmlEscape(e.id)
          << (e.live ? absl::StrCat(" (board #", e.board_rank, ")") : "")
          << "</text><g><title>" << HtmlEscape(tip) << "</title>"
          << "<rect x=" << left << " y=" << y - row / 2 << " width=" << width
          << " height=" << row << " fill=transparent />"
          << "<line x1=" << x(r.mu - 2 * r.sigma)
          << " x2=" << x(r.mu + 2 * r.sigma) << " y1=" << y << " y2=" << y
          << " stroke=\"" << ink(e) << "\" stroke-width=2 stroke-linecap=round "
          << "opacity=0.55 /><circle cx=" << x(r.mu) << " cy=" << y
          << " r=4.5 fill=\"" << ink(e)
          << "\" stroke=\"var(--surface)\" stroke-width=2 /></g>";
    }
    html
        << "<h2>Where each entry's skill converges</h2><p class=note>TrueSkill "
           "mu, the bar &plusmn;2&sigma;; bold rows are the versions on the "
           "board, with their place there.</p><svg viewBox=\"0 0 "
        << left + width + 20 << " " << top + row * by_mu.size() + 10 << "\">"
        << svg.str() << "</svg>";
  }

  // Each participant's versions along |at|, mu +-1 sigma; builtins as levels.
  const auto by_participant =
      [&](std::string_view title, std::string_view note,
          const std::function<double(const SwissEntry &)> &at,
          const std::vector<std::pair<double, std::string>> &ticks) {
        double first = ticks.front().first, last_x = ticks.back().first;
        for (const SwissEntry &e : entries_) {
          if (!e.participant.empty()) {
            first = std::min(first, at(e));
            last_x = std::max(last_x, at(e));
          }
        }
        const double left = 40, width = 680, top = 10, height = 260;
        const Scale x{first, last_x, left + 10, left + width - 90};
        const Scale y{lo, hi, top + height, top};
        std::ostringstream svg;
        YAxis(y, 5, left, width, svg);
        for (const auto &[at_tick, label] : ticks) {
          svg << "<text class=tick x=" << x(at_tick)
              << " y=" << top + height + 16 << " text-anchor=middle>"
              << HtmlEscape(label) << "</text>";
        }
        // Placed last, and nudged apart where two would overlap.
        struct Label {
          double y, x;
          std::string text;
        };
        std::vector<Label> labels;
        for (const SwissEntry &e : entries_) {
          if (!e.participant.empty()) {
            continue;
          }
          const double level = y(now.at(e.id).mu);
          svg << "<line x1=" << left << " x2=" << left + width - 90
              << " y1=" << level << " y2=" << level << " stroke=\""
              << kBuiltinInk
              << "\" stroke-width=1.5 stroke-dasharray=\"4 4\" />";
          labels.push_back({level + 4, left + width - 86, e.id});
        }
        for (const auto &[participant, index] : slot) {
          const std::string color = kSeries[index % std::size(kSeries)];
          std::string path;
          const SwissEntry *last = nullptr;
          std::ostringstream marks;
          for (const SwissEntry &e : entries_) {
            if (e.participant != participant) {
              continue;
            }
            const Rating &r = now.at(e.id);
            absl::StrAppend(&path, path.empty() ? "M" : "L", x(at(e)), " ",
                            y(r.mu));
            marks << "<g><title>" << HtmlEscape(e.id) << ": mu " << Fixed(r.mu)
                  << " sigma " << Fixed(r.sigma)
                  << "</title><line x1=" << x(at(e)) << " x2=" << x(at(e))
                  << " y1=" << y(r.mu - r.sigma) << " y2=" << y(r.mu + r.sigma)
                  << " stroke=\"" << color
                  << "\" stroke-width=1 opacity=0.6 /><circle cx=" << x(at(e))
                  << " cy=" << y(r.mu) << " r=4 fill=\"" << color
                  << "\" stroke=\"var(--surface)\" stroke-width=2 /></g>";
            last = &e;
          }
          svg << "<path d=\"" << path << "\" fill=none stroke=\"" << color
              << "\" stroke-width=2 />" << marks.str();
          if (last != nullptr) {
            labels.push_back(
                {y(now.at(last->id).mu) + 4, x(at(*last)) + 8, participant});
          }
        }
        std::sort(labels.begin(), labels.end(),
                  [](const Label &a, const Label &b) { return a.y < b.y; });
        for (std::size_t i = 0; i < labels.size(); ++i) {
          for (std::size_t j = 0; j < i; ++j) {
            if (std::abs(labels[i].x - labels[j].x) < 80 &&
                labels[i].y < labels[j].y + 12) {
              labels[i].y = labels[j].y + 12;
            }
          }
          svg << "<text class=lbl x=" << labels[i].x << " y=" << labels[i].y
              << ">" << HtmlEscape(labels[i].text) << "</text>";
        }
        html << "<h2>" << title << "</h2><p class=note>" << note
             << "</p><svg viewBox=\"0 0 " << left + width << " "
             << top + height + 24 << "\">" << svg.str() << "</svg>";
      };

  // 2. Did later versions get stronger?
  {
    int versions = 2;
    for (const SwissEntry &e : entries_) {
      versions = std::max(versions, e.version);
    }
    std::vector<std::pair<double, std::string>> ticks;
    for (int v = 1; v <= versions; ++v) {
      ticks.emplace_back(v, absl::StrCat("v", v));
    }
    by_participant(
        "Skill by version",
        "Each participant's versions in submission order, mu &plusmn;1&sigma;; "
        "dashed lines are the builtins. A curve that flattens or falls is "
        "iteration that stopped paying.",
        [](const SwissEntry &e) { return e.version; }, ticks);
  }
  // 2b. The same on one clock: who moved when, and whether they converged.
  {
    int64_t first = INT64_MAX, last = 0;
    for (const SwissEntry &e : entries_) {
      if (!e.participant.empty()) {
        first = std::min(first, e.submitted_unix_ms);
        last = std::max(last, e.submitted_unix_ms);
      }
    }
    std::vector<std::pair<double, std::string>> ticks;
    for (int i = 0; i <= 5; ++i) {
      const double t = first + (last - first) * i / 5.0;
      ticks.emplace_back(
          t, absl::FormatTime("%b %d %H:%M",
                              absl::FromUnixMillis(static_cast<int64_t>(t)),
                              absl::LocalTimeZone()));
    }
    by_participant(
        "Skill by submission time",
        "Every version where it was submitted, mu &plusmn;1&sigma;, all "
        "participants on one clock: whether they climbed together, and who "
        "stalled while the others moved.",
        [](const SwissEntry &e) {
          return static_cast<double>(e.submitted_unix_ms);
        },
        ticks);
  }

  // 3. Convergence: mu after each round, every entry.
  {
    const double left = 40, width = 680, top = 10, height = 260;
    const Scale x{0, static_cast<double>(std::max(1, rounds_)), left + 10,
                  left + width - 10};
    const Scale y{lo, hi, top + height, top};
    std::ostringstream svg;
    YAxis(y, 5, left, width, svg);
    for (int round = 0; round <= rounds_; ++round) {
      svg << "<text class=tick x=" << x(round) << " y=" << top + height + 16
          << " text-anchor=middle>" << round << "</text>";
    }
    // The live versions last, so they draw on top.
    std::vector<const SwissEntry *> order;
    for (const SwissEntry &e : entries_) {
      order.push_back(&e);
    }
    std::stable_partition(order.begin(), order.end(),
                          [](auto *e) { return !e->live; });
    for (const SwissEntry *e : order) {
      std::string path;
      for (std::size_t round = 0; round < snapshots_.size(); ++round) {
        absl::StrAppend(&path, round == 0 ? "M" : "L", x(round), " ",
                        y(snapshots_[round].at(e->id).mu));
      }
      svg << "<path d=\"" << path << "\" fill=none stroke=\"" << ink(*e)
          << "\" stroke-width=" << (e->live ? 2 : 1)
          << " opacity=" << (e->live ? 1 : 0.4)
          << (e->participant.empty() ? " stroke-dasharray=\"4 4\"" : "")
          << "><title>" << HtmlEscape(e->id) << "</title></path>";
    }
    html << "<h2>Convergence</h2><p class=note>Mu after each round; the live "
            "versions drawn heavier.</p><svg viewBox=\"0 0 "
         << left + width << " " << top + height + 24 << "\">" << svg.str()
         << "</svg>";
  }

  // The same numbers as a table.
  html << "<h2>Standings</h2><table><tr><th>#</th><th>entry</th><th>mu</th>"
          "<th>sigma</th><th>mu &minus; 3&sigma;</th><th>W</th><th>L</th>"
          "<th>board</th></tr>";
  for (std::size_t i = 0; i < by_mu.size(); ++i) {
    const SwissEntry &e = *by_mu[i];
    const Rating &r = now.at(e.id);
    const tournament_arena::Standing s = ratings_->Get(e.id);
    html << "<tr><td>" << i + 1 << "</td><td class=l>" << (e.live ? "<b>" : "")
         << HtmlEscape(e.id) << (e.live ? "</b>" : "") << "</td><td>"
         << Fixed(r.mu) << "</td><td>" << Fixed(r.sigma) << "</td><td>"
         << Fixed(r.Conservative()) << "</td><td>" << s.wins << "</td><td>"
         << s.losses << "</td><td>"
         << (e.live ? absl::StrCat("#", e.board_rank) : "") << "</td></tr>";
  }
  html << "</table>";

  html << "<h2>Rounds</h2>";
  for (std::size_t round = played_.size(); round-- > 0;) {
    html << "<h3>Round " << round + 1
         << "</h3><table><tr><th>a</th><th>b</th>"
            "<th>a's W-L</th><th>job</th></tr>";
    for (const Match &m : played_[round]) {
      const auto job = concluded_.find(m.job_id);
      html << "<tr><td class=l>" << HtmlEscape(m.a) << "</td><td class=l>"
           << HtmlEscape(m.b) << "</td><td>"
           << (job == concluded_.end() ? std::string("&hellip;")
               : job->second.error().empty()
                   ? absl::StrCat(job->second.wins(), "-", job->second.losses())
                   : absl::StrCat(job->second.wins(), "-", job->second.losses(),
                                  " (", HtmlEscape(job->second.error()), ")"))
           << "</td><td class=l><a href=\"/jobs/" << m.job_id << "\">"
           << m.job_id << "</a></td></tr>";
    }
    html << "</table>";
    if (!byes_[round].empty()) {
      html << "<p class=note>Bye: " << HtmlEscape(byes_[round]) << "</p>";
    }
  }
  html << "</div></body></html>";
  return std::pair{std::string("text/html; charset=utf-8"), html.str()};
}

}  // namespace tournament_arena
