#ifndef GAME_ARENA_GAME_ARENA_SERVER_DASHBOARD_H
#define GAME_ARENA_GAME_ARENA_SERVER_DASHBOARD_H

// /jobs, /participants, /games. Unauthenticated: source shows only if all may.

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "game_arena/server/candidate_store.h"
#include "game_arena/server/job_log.h"
#include "game_arena/standings/game_history.h"
#include "game_arena/standings/standings.h"

namespace tournament_arena {

// What a problem ships for its replays (arena_problem's replay_assets): any
// files, served at /assets/<name>, and the ES module among them that draws a
// step's view in the browser. Without a module, views are shown as text.
struct ReplayAssets {
  std::map<std::string, std::string, std::less<>> files;  // name -> bytes
  std::string module;
};

class Dashboard {
 public:
  // |standings| may be null.
  Dashboard(const CandidateStore* candidates, const JobLog* jobs,
            const tournament_broker::GameHistory* games,
            const Standings* standings, bool show_source,
            ReplayAssets assets = {});

  // The content type and body, or nullopt for a 404.
  std::optional<std::pair<std::string, std::string>> Route(
      std::string_view target) const;

 private:
  std::string JobsPage() const;
  std::optional<std::string> JobPage(const std::string& job_id) const;
  std::optional<std::string> ParticipantPage(const std::string& id) const;
  // Its files, and why it failed to build if it did.
  std::string CodeOf(const proto::Candidate& candidate) const;
  std::string GamesPage(int page, const std::string& player) const;
  std::optional<std::string> ReplayPage(const std::string& game_id) const;

  const CandidateStore* candidates_;             // not owned
  const JobLog* jobs_;                           // not owned
  const tournament_broker::GameHistory* games_;  // not owned
  const Standings* standings_;                   // not owned, may be null
  const bool show_source_;
  const ReplayAssets assets_;
};

}  // namespace tournament_arena

#endif  // GAME_ARENA_GAME_ARENA_SERVER_DASHBOARD_H
