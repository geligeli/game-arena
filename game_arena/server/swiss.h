#ifndef GAME_ARENA_GAME_ARENA_SERVER_SWISS_H
#define GAME_ARENA_GAME_ARENA_SERVER_SWISS_H

// A Swiss re-rank: every version a job log holds, and the builtins, played in
// rounds of neighbours by TrueSkill, a group to each game's seats, on the
// fleet like any other match.

#include <condition_variable>
#include <cstdint>
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
#include "game_arena/server/candidate_store.h"
#include "game_arena/server/job_log.h"
#include "game_arena/server/scheduler.h"
#include "game_arena/standings/trueskill.h"
#include "game_arena/standings/trueskill_standings.h"

namespace tournament_arena {

struct SwissRound {
  std::vector<std::vector<std::string>> groups;
  std::vector<std::string> byes;  // what a whole number of groups leaves over
};

// |ranked| is strongest first; a group has |seats| entries. Each takes the
// nearest entries below its first that have met none of its members, a
// rematch only when none is left; the entries a whole number of groups leaves
// over sit out, the lowest without a bye first. |played| holds (lo, hi) pairs.
SwissRound SwissGroups(
    const std::vector<std::string> &ranked,
    const std::set<std::pair<std::string, std::string>> &played,
    const std::set<std::string> &had_bye, std::size_t seats);

struct SwissEntry {
  std::string id;           // "<participant>-vNN", or "builtin:<spec>"
  std::string participant;  // empty for a builtin
  int version = 0;          // 1-based per participant; 0 for a builtin
  bool live = false;        // the participant's last: the one on the board
  int board_rank = 0;       // the live one's place on |board|, 1-based
  int64_t submitted_unix_ms = 0;
};

// Every version in |jobs| that played a game, oldest first and without
// byte-identical repeats, stored in |store| as a READY candidate of its own:
// its patch moved from <submit_dir>/<participant>/ to <submit_dir>/<id>/, so
// two versions of one participant can meet in one order.
std::vector<SwissEntry> SeedVersions(const JobLog &jobs,
                                     const std::string &submit_dir,
                                     const Standings &board,
                                     CandidateStore *store);

class SwissRun {
 public:
  // |rounds| <= 0: ceil(log2(entries)) + 3, counting the rounds a run on the
  // same data dir played before: |state| is where it keeps them, and |jobs|
  // how their matches ended.
  SwissRun(std::vector<SwissEntry> entries, int rounds, int games, int seats,
           std::string game, Scheduler *scheduler,
           const CandidateView *candidates, const TrueSkillStandings *ratings,
           const JobLog *jobs, std::filesystem::path state);
  ~SwissRun();

  void Start();
  // The scheduler's on_concluded, under its lock: must not call back into it.
  void OnConcluded(const proto::Job &job);
  // "/swiss": the run's progress page.
  std::optional<std::pair<std::string, std::string>> Route(
      std::string_view target) const;

 private:
  struct Match {
    std::vector<std::string> members;
    std::string job_id;
  };

  void Run();
  // Reads back |state_|; under mutex_.
  void Resume(std::set<std::pair<std::string, std::string>> *played,
              std::set<std::string> *had_bye);
  std::map<std::string, tournament_broker::trueskill::Rating> Snapshot() const;

  const std::vector<SwissEntry> entries_;
  const int rounds_;
  const int games_;
  const int seats_;
  const std::string game_;
  Scheduler *scheduler_;               // not owned
  const CandidateView *candidates_;    // not owned
  const TrueSkillStandings *ratings_;  // not owned
  const JobLog *jobs_;                 // not owned
  const std::filesystem::path state_;

  mutable std::mutex mutex_;
  std::condition_variable cv_;
  bool stopping_ = false;
  std::vector<std::vector<Match>> played_;
  std::vector<std::vector<std::string>> byes_;
  std::map<std::string, proto::Job> concluded_;  // by job id
  std::thread thread_;
};

}  // namespace tournament_arena

#endif  // GAME_ARENA_GAME_ARENA_SERVER_SWISS_H
