#include "game_arena/server/skill_charts.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdio>
#include <sstream>

#include "absl/strings/str_cat.h"
#include "absl/time/time.h"
#include "game_arena/standings/http_leaderboard.h"

namespace tournament_arena {

namespace {

using tournament_broker::HtmlEscape;
using tournament_broker::trueskill::Rating;

// The reference categorical order (light), by participant; builtins are grey.
constexpr const char *kSeries[] = {"#2a78d6", "#eb6834", "#1baf7a", "#eda100",
                                   "#e87ba4", "#008300", "#4a3aa7", "#e34948"};
constexpr const char *kGrey = "#898781";

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

}  // namespace

const std::string_view kSkillChartStyle = R"(<style>
.viz{--surface:#fcfcfb;--ink:#0b0b0b;--ink2:#52514e;--muted:#898781;
--grid:#e1e0d9;--axis:#c3c2b7;background:var(--surface);color:var(--ink);
font-family:system-ui,-apple-system,"Segoe UI",sans-serif;max-width:900px}
.viz svg{display:block;width:100%;height:auto;overflow:visible}
.viz .grid{stroke:var(--grid);stroke-width:1}
.viz .tick{fill:var(--muted);font-size:11px;font-variant-numeric:tabular-nums}
.viz .lbl{fill:var(--ink2);font-size:11px}
.viz .live{fill:var(--ink);font-weight:600}
.viz .dim{fill:var(--muted)}
.viz h2{font-size:16px;margin:28px 0 4px}
.viz p.note{color:var(--ink2);font-size:13px;margin:0 0 8px}
.legend{display:flex;gap:16px;font-size:12px;color:var(--ink2);margin:4px 0}
.legend i{display:inline-block;width:10px;height:10px;border-radius:5px;
margin-right:5px;vertical-align:-1px}
.viz circle,.viz path.hit{cursor:default}
td.l{text-align:left}
</style>)";

std::string Fixed(double value, int digits) {
  char text[32];
  std::snprintf(text, sizeof(text), "%.*f", digits, value);
  return text;
}

std::string SkillCharts(const std::vector<ChartEntry> &entries,
                        std::string_view converge_note) {
  std::map<std::string, int> slot;
  for (const ChartEntry &entry : entries) {
    if (!entry.participant.empty() && !slot.contains(entry.participant)) {
      const int next = static_cast<int>(slot.size());
      slot[entry.participant] = next;
    }
  }
  const auto ink = [&](const ChartEntry &entry) -> std::string {
    return entry.participant.empty() || entry.dim
               ? kGrey
               : kSeries[slot.at(entry.participant) % std::size(kSeries)];
  };
  std::vector<const ChartEntry *> by_mu;
  for (const ChartEntry &entry : entries) {
    by_mu.push_back(&entry);
  }
  std::stable_sort(by_mu.begin(), by_mu.end(), [](auto *a, auto *b) {
    return a->rating.mu > b->rating.mu;
  });
  double lo = 1e9, hi = -1e9;
  const auto widen = [&](const Rating &r) {
    lo = std::min(lo, r.mu - 2 * r.sigma);
    hi = std::max(hi, r.mu + 2 * r.sigma);
  };
  for (const ChartEntry &entry : entries) {
    widen(entry.rating);
  }
  if (lo > hi) {
    lo = 0;
    hi = 50;
  }
  lo = std::floor(lo / 5) * 5;
  hi = std::ceil(hi / 5) * 5;

  std::ostringstream html;
  html << "<div class=legend>";
  for (const auto &[participant, index] : slot) {
    html << "<span><i style=\"background:"
         << kSeries[index % std::size(kSeries)] << "\"></i>"
         << HtmlEscape(participant) << "</span>";
  }
  html << "<span><i style=\"background:" << kGrey
       << "\"></i>builtin or out</span></div>";

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
      const ChartEntry &e = *by_mu[i];
      const Rating &r = e.rating;
      const double y = top + row * i + row / 2;
      const std::string tip = absl::StrCat(
          e.id, ": mu ", Fixed(r.mu), " sigma ", Fixed(r.sigma), ", ", e.wins,
          "-", e.losses, e.note.empty() ? "" : absl::StrCat(", ", e.note));
      svg << "<text class=\"lbl" << (e.bold ? " live" : "")
          << (e.dim ? " dim" : "") << "\" x=" << left - 8 << " y=" << y + 4
          << " text-anchor=end>" << i + 1 << ". " << HtmlEscape(e.id)
          << (e.note.empty() ? "" : absl::StrCat(" (", HtmlEscape(e.note), ")"))
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
    html << "<h2>Where each entry's skill converges</h2><p class=note>"
         << converge_note << "</p><svg viewBox=\"0 0 " << left + width + 20
         << " " << top + row * by_mu.size() + 10 << "\">" << svg.str()
         << "</svg>";
  }

  // 2. Every version where it was submitted, on one clock, mu +-1 sigma: who
  // moved when, and whether they converged; builtins as levels.
  {
    int64_t from = INT64_MAX, to = 0;
    for (const ChartEntry &e : entries) {
      if (!e.participant.empty()) {
        from = std::min(from, e.submitted_unix_ms);
        to = std::max(to, e.submitted_unix_ms);
      }
    }
    if (from > to) {
      from = to = 0;
    }
    std::vector<std::pair<double, std::string>> ticks;
    for (int i = 0; i <= 5; ++i) {
      const double t = from + (to - from) * i / 5.0;
      ticks.emplace_back(
          t, absl::FormatTime("%b %d %H:%M",
                              absl::FromUnixMillis(static_cast<int64_t>(t)),
                              absl::LocalTimeZone()));
    }
    const auto at = [](const ChartEntry &e) {
      return static_cast<double>(e.submitted_unix_ms);
    };
    const double left = 40, width = 680, top = 10, height = 260;
    const Scale x{static_cast<double>(from), static_cast<double>(to), left + 10,
                  left + width - 90};
    const Scale y{lo, hi, top + height, top};
    std::ostringstream svg;
    YAxis(y, 5, left, width, svg);
    for (const auto &[at_tick, label] : ticks) {
      svg << "<text class=tick x=" << x(at_tick) << " y=" << top + height + 16
          << " text-anchor=middle>" << HtmlEscape(label) << "</text>";
    }
    // Placed last, and nudged apart where two would overlap.
    struct Label {
      double y, x;
      std::string text;
    };
    std::vector<Label> labels;
    for (const ChartEntry &e : entries) {
      if (!e.participant.empty()) {
        continue;
      }
      const double level = y(e.rating.mu);
      svg << "<line x1=" << left << " x2=" << left + width - 90
          << " y1=" << level << " y2=" << level << " stroke=\"" << kGrey
          << "\" stroke-width=1.5 stroke-dasharray=\"4 4\" />";
      labels.push_back({level + 4, left + width - 86, e.id});
    }
    for (const auto &[participant, index] : slot) {
      const std::string color = kSeries[index % std::size(kSeries)];
      std::string path;
      const ChartEntry *last = nullptr;
      std::ostringstream marks;
      for (const ChartEntry &e : entries) {
        if (e.participant != participant) {
          continue;
        }
        const Rating &r = e.rating;
        absl::StrAppend(&path, path.empty() ? "M" : "L", x(at(e)), " ",
                        y(r.mu));
        marks << "<g><title>" << HtmlEscape(e.id) << ": mu " << Fixed(r.mu)
              << " sigma " << Fixed(r.sigma) << "</title><line x1=" << x(at(e))
              << " x2=" << x(at(e)) << " y1=" << y(r.mu - r.sigma)
              << " y2=" << y(r.mu + r.sigma) << " stroke=\"" << color
              << "\" stroke-width=1 opacity=0.6 /><circle cx=" << x(at(e))
              << " cy=" << y(r.mu) << " r=4 fill=\"" << ink(e)
              << "\" stroke=\"var(--surface)\" stroke-width=2 /></g>";
        last = &e;
      }
      svg << "<path d=\"" << path << "\" fill=none stroke=\"" << color
          << "\" stroke-width=2 />" << marks.str();
      if (last != nullptr) {
        labels.push_back(
            {y(last->rating.mu) + 4, x(at(*last)) + 8, participant});
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
      svg << "<text class=lbl x=" << labels[i].x << " y=" << labels[i].y << ">"
          << HtmlEscape(labels[i].text) << "</text>";
    }
    html << "<h2>Skill by submission time</h2><p class=note>Every version "
            "where it was submitted, mu &plusmn;1&sigma;, all participants on "
            "one clock: whether they climbed together, and who stalled while "
            "the others moved.</p><svg viewBox=\"0 0 "
         << left + width << " " << top + height + 24 << "\">" << svg.str()
         << "</svg>";
  }

  return html.str();
}

}  // namespace tournament_arena
