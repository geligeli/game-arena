#ifndef GAME_ARENA_GAME_ARENA_SERVER_SKILL_CHARTS_H
#define GAME_ARENA_GAME_ARENA_SERVER_SKILL_CHARTS_H

// Where TrueSkill puts each version, as inline SVG: the Swiss re-rank's page
// (/swiss) and continuous matchmaking's (/pool) draw the same four charts.
//
// 1. Where each skill converges: every entry by mu, a 2-sigma bar.
// 2. Skill by submission time: every version on one clock.
// 3. Convergence: mu over |history|.

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "game_arena/standings/trueskill.h"

namespace tournament_arena {

struct ChartEntry {
  std::string id;
  std::string participant;  // empty for a builtin, drawn grey and dashed
  int version = 0;          // 1-based per participant
  int64_t submitted_unix_ms = 0;
  tournament_broker::trueskill::Rating rating;
  int wins = 0;
  int losses = 0;
  bool bold = false;  // drawn heavier and labelled bold
  bool dim = false;   // greyed: out of the running
  std::string note;   // after its label, e.g. "board #2"
};

// Every entry's rating at one point of |x|.
struct ChartSnapshot {
  double x = 0;
  std::map<std::string, tournament_broker::trueskill::Rating> ratings;
};

// The charts' style, once per page, and a number as the charts print it.
extern const std::string_view kSkillChartStyle;
std::string Fixed(double value, int digits = 1);

struct ChartNotes {
  std::string converge;     // under "Where each entry's skill converges"
  std::string convergence;  // under "Convergence"
};

// The legend and the four charts, as HTML, for inside a `.viz` div.
// |history_ticks| label chart 4's x axis.
std::string SkillCharts(
    const std::vector<ChartEntry> &entries,
    const std::vector<ChartSnapshot> &history,
    const std::vector<std::pair<double, std::string>> &history_ticks,
    const ChartNotes &notes);

}  // namespace tournament_arena

#endif  // GAME_ARENA_GAME_ARENA_SERVER_SKILL_CHARTS_H
