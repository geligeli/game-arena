#ifndef GAME_ARENA_GAME_ARENA_SERVER_MATCHMAKING_H
#define GAME_ARENA_GAME_ARENA_SERVER_MATCHMAKING_H

// Continuous matches between submissions (problem.proto's Matchmaking): the
// fleet never idles while a rating is uncertain.
//
// The pool is every READY version that is plausibly in the top |pool|: its
// mu + 2 sigma reaches the |pool|-th best mu - 2 sigma. The rest have dropped
// out and are not scheduled, so the work grows with the pool, not with every
// version ever submitted; and a version stays until its own games show it
// out, however new. Whenever nothing is queued and a slot is free, the least
// certain member whose place is still open (sigma, discounted by the matches
// it already has running) plays the rivals, one per other seat, that maximise
// TrueSkill's match quality times their combined variance, skipping its last
// few opponents and, with more than two seats, preferring rivals of different
// authors. Placement jobs are queued, so they never wait behind a match.
//
// <data_dir>/matchmaking.tsv keeps, across restarts, every finished match
// ("G  <unix ms>  <job>  <games>  <a>,<b>,...  <a's finishes, first first>"),
// for /pool.

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "game_arena/proto/arena.pb.h"
#include "game_arena/proto/problem.pb.h"
#include "game_arena/server/candidate_store.h"
#include "game_arena/server/scheduler.h"
#include "game_arena/server/skill_charts.h"
#include "game_arena/standings/trueskill.h"
#include "game_arena/standings/trueskill_standings.h"

namespace tournament_arena {

struct PoolMember {
  std::string id;
  std::string author;  // the id itself for a builtin or a version with none
  tournament_broker::trueskill::Rating rating;
  int running = 0;  // matches of its in flight
  // Surely in the top: no version outside it could plausibly overtake it.
  bool settled = false;
};

// The pool out of every READY version, as above. Best first, by mu - 2 sigma.
std::vector<PoolMember> PoolOf(std::vector<PoolMember> rated, int size);

// The next match, the member it is for first, or nullopt with fewer members
// than |seats|. |recent| maps a member to its last opponents, which it does
// not meet again while another group is left. With more than two seats a
// group whose authors differ goes before one that has recent opponents: a
// third seat is what two versions of one author could gang up on.
std::optional<std::vector<std::string>> ChooseGroup(
    const std::vector<PoolMember> &pool,
    const std::map<std::string, std::deque<std::string>> &recent,
    const tournament_broker::trueskill::Params &params, std::size_t seats);

class Matchmaker {
 public:
  // |builtins| are drawn as levels on the charts, never matched.
  Matchmaker(proto::Matchmaking config, std::string game, int seats,
             std::vector<std::string> builtins, Scheduler *scheduler,
             const CandidateStore *candidates,
             const TrueSkillStandings *ratings,
             tournament_broker::trueskill::Params params,
             std::filesystem::path state);
  ~Matchmaker();

  void Start();
  // The scheduler's on_concluded, under its lock: must not call back into it.
  void OnConcluded(const proto::Job &job);
  // "/pool": the pool, its matches and the charts.
  std::optional<std::pair<std::string, std::string>> Route(
      std::string_view target) const;
  // Ids in the pool now.
  std::set<std::string> Members() const;

 private:
  struct Match {
    std::vector<std::string> members = {};  // the one it was for first
    int64_t unix_ms = 0;
    int games = 0;
    std::vector<int> finishes = {};  // the first member's, first place first
    std::string job_id;
  };

  void Run();
  void TopUp();
  void Load();
  // Every READY version, rated.
  std::vector<PoolMember> Rated(
      const std::map<std::string, int> &running) const;
  void Append(const std::string &line) const;

  const proto::Matchmaking config_;
  const std::string game_;
  const std::size_t seats_;
  const std::vector<std::string> builtins_;
  Scheduler *scheduler_;               // not owned
  const CandidateStore *candidates_;   // not owned
  const TrueSkillStandings *ratings_;  // not owned
  const tournament_broker::trueskill::Params params_;
  const std::filesystem::path state_;

  mutable std::mutex mutex_;
  std::condition_variable wake_;
  bool stopping_ = false;
  std::map<std::string, Match> running_;  // by job id
  std::set<std::string> early_;  // concluded before TopUp recorded them
  std::deque<Match> finished_;   // newest first, the last 30
  std::map<std::string, std::deque<std::string>> recent_;
  int64_t matches_done_ = 0;
  std::thread thread_;
};

}  // namespace tournament_arena

#endif  // GAME_ARENA_GAME_ARENA_SERVER_MATCHMAKING_H
