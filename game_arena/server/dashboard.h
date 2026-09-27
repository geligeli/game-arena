#ifndef GAME_ARENA_GAME_ARENA_SERVER_DASHBOARD_H
#define GAME_ARENA_GAME_ARENA_SERVER_DASHBOARD_H

// /jobs, /participants, /games. Unauthenticated: source shows only if all may.

#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "game_arena/server/candidate_store.h"
#include "game_arena/server/job_log.h"
#include "game_arena/standings/game_history.h"
#include "game_arena/standings/standings.h"

namespace tournament_arena {

class Dashboard {
 public:
  // |standings| may be null.
  Dashboard(const CandidateStore *candidates, const JobLog *jobs,
            const tournament_broker::GameHistory *games,
            const Standings *standings, bool show_source);

  // The content type and body, or nullopt for a 404.
  std::optional<std::pair<std::string, std::string>> Route(
      std::string_view target) const;

 private:
  std::string JobsPage() const;
  std::optional<std::string> JobPage(const std::string &job_id) const;
  std::optional<std::string> ParticipantPage(const std::string &id) const;
  std::string GamesPage(int page, const std::string &player) const;
  std::optional<std::string> ReplayPage(const std::string &game_id) const;

  const CandidateStore *candidates_;             // not owned
  const JobLog *jobs_;                           // not owned
  const tournament_broker::GameHistory *games_;  // not owned
  const Standings *standings_;                   // not owned, may be null
  const bool show_source_;
};

}  // namespace tournament_arena

#endif  // GAME_ARENA_GAME_ARENA_SERVER_DASHBOARD_H
