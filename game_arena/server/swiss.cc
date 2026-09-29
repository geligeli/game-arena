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
#include "absl/strings/str_split.h"
#include "absl/time/time.h"
#include "game_arena/server/skill_charts.h"
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
    // A job log from a versioned coordinator names versions; its author is
    // always the participant.
    const std::string participant = submission.author().empty()
                                        ? submission.candidate_id()
                                        : submission.author();
    // The participant's own patch, whichever directory it was stored under.
    const std::string patch = MovePatchDir(
        submission.patch(), submit_dir, submission.candidate_id(), participant);
    // A match's record carries no patch: only submissions are versions.
    if (!played || submission.patch().empty() ||
        !seen[participant].insert(patch).second) {
      continue;
    }
    char id[64];
    std::snprintf(id, sizeof(id), "%s-v%02zu", participant.c_str(),
                  seen[participant].size());

    // A versioned store names and moves the version itself; otherwise the
    // version is made to look like a participant of its own.
    proto::SubmitRequest request;
    request.set_author(store->versions() ? participant : id);
    request.set_display_name(id);
    request.set_game(submission.game());
    request.set_patch(store->versions()
                          ? patch
                          : MovePatchDir(patch, submit_dir, participant, id));
    request.set_entry_header(submission.entry_header());
    *request.mutable_extra_deps() = submission.extra_deps();
    *request.mutable_params() = submission.params();
    std::string error;
    // Already there when a run resumes on its own data dir.
    std::string made = id;
    if (!store->Get(id).has_value()) {
      const std::optional<proto::Candidate> created =
          store->Create(request, &error);
      if (!created.has_value()) {
        LOG(WARNING) << "Swiss: skipping " << id << " (job "
                     << record.job().job_id() << "): " << error;
        continue;
      }
      made = created->candidate_id();
    }
    store->Backdate(made, submission.submitted_unix_ms());
    SwissEntry &entry = entries.emplace_back();
    entry.id = made;
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
  {
    // A seed is placed, and built, like any submission; one that fails to
    // build sits out.
    std::unique_lock lock(mutex_);
    const auto status = [&](const std::string &id) {
      return IsBuiltin(id) ? proto::Candidate::READY
                           : candidates_->Get(id)->status();
    };
    cv_.wait(lock, [&] {
      return stopping_ || std::ranges::none_of(ids, [&](const auto &id) {
               return status(id) == proto::Candidate::PENDING;
             });
    });
    if (stopping_) {
      return;
    }
    std::erase_if(ids, [&](const auto &id) {
      return status(id) != proto::Candidate::READY;
    });
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
  std::vector<ChartEntry> entries;
  for (const SwissEntry &e : entries_) {
    const Standing standing = ratings_->Get(e.id);
    entries.push_back(
        {.id = e.id,
         .participant = e.participant,
         .version = e.version,
         .submitted_unix_ms = e.submitted_unix_ms,
         .rating = now.at(e.id),
         .wins = standing.wins,
         .losses = standing.losses,
         .bold = e.live,
         .note = e.live ? absl::StrCat("board #", e.board_rank) : ""});
  }
  std::vector<ChartSnapshot> history;
  for (std::size_t round = 0; round < snapshots_.size(); ++round) {
    history.push_back({static_cast<double>(round), snapshots_[round]});
  }
  std::vector<std::pair<double, std::string>> ticks;
  for (int round = 0; round <= rounds_; ++round) {
    ticks.emplace_back(round, std::to_string(round));
  }
  std::vector<const SwissEntry *> by_mu;
  for (const SwissEntry &entry : entries_) {
    by_mu.push_back(&entry);
  }
  std::sort(by_mu.begin(), by_mu.end(), [&](auto *a, auto *b) {
    return now.at(a->id).mu > now.at(b->id).mu;
  });

  std::ostringstream html;
  html << tournament_broker::PageStart("Swiss re-rank", /*refresh=*/true)
       << kSkillChartStyle << "<div class=viz>";
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
  html << SkillCharts(
      entries, history, ticks,
      {.converge = "TrueSkill mu, the bar &plusmn;2&sigma;; bold rows are the "
                   "versions on the board, with their place there.",
       .convergence = "Mu after each round; the live versions drawn heavier."});

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
