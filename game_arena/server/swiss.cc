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
#include "absl/strings/str_join.h"
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

void AddPairs(const std::vector<std::string> &group,
              std::set<std::pair<std::string, std::string>> *played) {
  for (const std::string &a : group) {
    for (const std::string &b : group) {
      if (a < b) {
        played->insert(Key(a, b));
      }
    }
  }
}

}  // namespace

SwissRound SwissGroups(
    const std::vector<std::string> &ranked,
    const std::set<std::pair<std::string, std::string>> &played,
    const std::set<std::string> &had_bye, std::size_t seats) {
  SwissRound round;
  std::vector<std::string> pool = ranked;
  while (pool.size() % seats != 0) {
    auto sits = std::find_if(pool.rbegin(), pool.rend(), [&](const auto &id) {
      return !had_bye.contains(id);
    });
    if (sits == pool.rend()) {
      sits = pool.rbegin();
    }
    round.byes.push_back(*sits);
    pool.erase(std::next(sits).base());
  }
  std::vector<bool> used(pool.size(), false);
  for (std::size_t i = 0; i < pool.size(); ++i) {
    if (used[i]) {
      continue;
    }
    used[i] = true;
    std::vector<std::string> &group = round.groups.emplace_back();
    group.push_back(pool[i]);
    while (group.size() < seats) {
      std::size_t pick = pool.size();
      for (std::size_t j = i + 1; j < pool.size(); ++j) {
        if (used[j]) {
          continue;
        }
        if (pick == pool.size()) {
          pick = j;  // the rematch to fall back on
        }
        if (std::ranges::none_of(group, [&](const std::string &member) {
              return played.contains(Key(member, pool[j]));
            })) {
          pick = j;
          break;
        }
      }
      used[pick] = true;
      group.push_back(pool[pick]);
    }
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
                   int seats, std::string game, Scheduler *scheduler,
                   const CandidateView *candidates,
                   const TrueSkillStandings *ratings, const JobLog *jobs,
                   std::filesystem::path state)
    : entries_(std::move(entries)),
      rounds_(rounds > 0 ? rounds
                         : static_cast<int>(std::ceil(std::log2(
                               std::max<std::size_t>(2, entries_.size())))) +
                               3),
      games_(games),
      seats_(seats),
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

// State lines, tab-separated: "M <round> <member>... <job id>" as a match is
// queued and "B <round> <id>" for each bye.
void SwissRun::Resume(std::set<std::pair<std::string, std::string>> *played,
                      std::set<std::string> *had_bye) {
  std::ifstream in(state_);
  for (std::string line; std::getline(in, line);) {
    const std::vector<std::string> f = absl::StrSplit(line, '\t');
    const std::size_t round = std::stoul(f[1]);
    if (f[0] == "M") {
      const std::string &job_id = f.back();
      played_.resize(std::max(played_.size(), round));
      played_[round - 1].push_back({{f.begin() + 2, f.end() - 1}, job_id});
      AddPairs(played_[round - 1].back().members, played);
      const auto record = jobs_->Get(job_id);
      proto::Job job = record.has_value() ? record->job() : proto::Job();
      if (job.state() != proto::Job::DONE &&
          job.state() != proto::Job::FAILED) {
        job.set_job_id(job_id);
        job.set_state(proto::Job::CANCELLED);
        job.set_error("dropped: the run stopped before it finished");
      }
      concluded_[job_id] = job;
    } else if (f[0] == "B") {
      byes_.resize(std::max(byes_.size(), round));
      byes_[round - 1].push_back(f[2]);
      had_bye->insert(f[2]);
    }
  }
  byes_.resize(played_.size());
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
    const SwissRound drawn = SwissGroups(ids, played, had_bye, seats_);
    std::vector<Match> matches;
    for (std::vector<std::string> group : drawn.groups) {
      AddPairs(group, &played);
      // The candidate's side is one with code, when any has.
      std::ranges::stable_partition(
          group, [](const std::string &id) { return !IsBuiltin(id); });
      proto::Candidate candidate;
      if (IsBuiltin(group[0])) {
        candidate.set_candidate_id(group[0]);
        candidate.set_game(game_);
      } else {
        candidate = *candidates_->Get(group[0]);
      }
      std::vector<std::string> opponents;
      for (auto it = group.begin() + 1; it != group.end(); ++it) {
        opponents.push_back(IsBuiltin(*it) ? *it : "player:" + *it);
      }
      matches.push_back(
          {group, scheduler_->EnqueueMatch(candidate, opponents, games_)});
      std::ofstream(state_, std::ios::app)
          << "M\t" << round + 1 << "\t" << absl::StrJoin(group, "\t") << "\t"
          << matches.back().job_id << "\n";
    }
    for (const std::string &bye : drawn.byes) {
      had_bye.insert(bye);
      std::ofstream(state_, std::ios::app)
          << "B\t" << round + 1 << "\t" << bye << "\n";
    }
    LOG(INFO) << "Swiss: round " << round + 1 << " of " << rounds_ << ", "
              << matches.size() << " match(es)";

    std::unique_lock lock(mutex_);
    played_.push_back(matches);
    byes_.push_back(drawn.byes);
    cv_.wait(lock, [&] {
      return stopping_ ||
             std::all_of(matches.begin(), matches.end(), [&](const Match &m) {
               return concluded_.contains(m.job_id);
             });
    });
    if (stopping_) {
      return;
    }
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
      static_cast<int>(played_.size()) == rounds_ && done == total;
  html << "<p>" << (finished ? "Finished: " : "Round ")
       << (finished ? rounds_ : static_cast<int>(played_.size())) << " of "
       << rounds_ << " &middot; " << entries_.size() << " entries &middot; "
       << done << " of " << total << " matches done (" << running
       << " running, " << queued << " queued) &middot; " << workers
       << " worker(s) &middot; " << games_ << " games per match</p>";
  html << SkillCharts(entries,
                      "TrueSkill mu, the bar &plusmn;2&sigma;; bold rows are "
                      "the versions on the board, with their place there.");

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
         << "</h3><table><tr><th>match</th>"
            "<th>first's W-L</th><th>job</th></tr>";
    for (const Match &m : played_[round]) {
      const auto job = concluded_.find(m.job_id);
      html << "<tr><td class=l>" << HtmlEscape(absl::StrJoin(m.members, " vs "))
           << "</td><td>"
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
      html << "<p class=note>Bye: "
           << HtmlEscape(absl::StrJoin(byes_[round], ", ")) << "</p>";
    }
  }
  html << "</div></body></html>";
  return std::pair{std::string("text/html; charset=utf-8"), html.str()};
}

}  // namespace tournament_arena
