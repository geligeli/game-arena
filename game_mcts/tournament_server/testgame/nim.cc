#include "game_mcts/tournament_server/testgame/nim.h"

#include <algorithm>
#include <charconv>
#include <memory>
#include <random>
#include <string>
#include <string_view>

#include "absl/log/check.h"

namespace arena_testgame {
namespace {

auto ParseInt(std::string_view text, int *out) -> bool {
  if (text.empty()) {
    return false;
  }
  const char *begin = text.data();
  const char *end = begin + text.size();
  const std::from_chars_result result = std::from_chars(begin, end, *out);
  return result.ec == std::errc{} && result.ptr == end;
}

}  // namespace

auto ParseState(std::string_view bytes, int *remaining, int *player) -> bool {
  const std::size_t colon = bytes.find(':');
  if (colon == std::string_view::npos) {
    return false;
  }
  if (!ParseInt(bytes.substr(0, colon), remaining) ||
      !ParseInt(bytes.substr(colon + 1), player)) {
    return false;
  }
  return *remaining >= 0 && (*player == 0 || *player == 1);
}

auto NimSession::SerializeState() const -> std::string {
  return std::to_string(remaining_) + ":" + std::to_string(player_);
}

void NimSession::ApplyChanceAction(std::mt19937 & /*gen*/) {
  // Unreachable: IsChanceNode() is always false. Loud rather than silent,
  // because reaching it would mean the broker ignored IsChanceNode().
  CHECK(false) << "Nim has no chance nodes";
}

auto NimSession::ApplySerializedAction(std::string_view bytes,
                                       std::string *error) -> bool {
  int take = 0;
  if (!ParseInt(bytes, &take)) {
    *error = "action bytes do not parse as an integer";
    return false;
  }
  if (take < 1 || take > kMaxTake) {
    *error = "take must be between 1 and " + std::to_string(kMaxTake);
    return false;
  }
  if (take > remaining_) {
    *error = "cannot take " + std::to_string(take) + " of " +
             std::to_string(remaining_) + " remaining";
    return false;
  }
  RecordStep(player_, std::string(bytes));
  remaining_ -= take;
  if (remaining_ == 0) {
    winner_ = player_;  // normal play: taking the last stone wins
  } else {
    player_ = 1 - player_;
  }
  return true;
}

auto NimSession::Outcome() const
    -> std::optional<tournament_broker::GameOutcome> {
  if (winner_ < 0) {
    return std::nullopt;
  }
  return tournament_broker::GameOutcome{.is_draw = false,
                                        .winning_player = winner_};
}

auto MakeBuiltin(std::string_view spec, std::string *error)
    -> std::optional<tournament_broker::BuiltinFn> {
  if (spec == "random") {
    return [](std::string_view state_bytes, std::mt19937 &gen) -> std::string {
      int remaining = 0;
      int player = 0;
      if (!ParseState(state_bytes, &remaining, &player) || remaining <= 0) {
        return "1";  // let the referee reject it
      }
      const int most = std::min(kMaxTake, remaining);
      std::uniform_int_distribution<int> pick(1, most);
      return std::to_string(pick(gen));
    };
  }
  if (spec == "optimal") {
    return [](std::string_view state_bytes, std::mt19937 &gen) -> std::string {
      int remaining = 0;
      int player = 0;
      if (!ParseState(state_bytes, &remaining, &player) || remaining <= 0) {
        return "1";
      }
      // Leave a multiple of (kMaxTake + 1) behind and the opponent is lost.
      // From such a position there is no winning move, so play uniformly.
      const int winning = remaining % (kMaxTake + 1);
      if (winning != 0) {
        return std::to_string(winning);
      }
      std::uniform_int_distribution<int> pick(1, std::min(kMaxTake, remaining));
      return std::to_string(pick(gen));
    };
  }
  *error = "unknown builtin '" + std::string(spec) + "' (want random|optimal)";
  return std::nullopt;
}

auto Descriptor() -> tournament_broker::GameDescriptor {
  return tournament_broker::GameDescriptor{
      .name = "nim",
      .new_session = [] { return std::make_unique<NimSession>(); },
      .make_builtin = &MakeBuiltin,
  };
}

}  // namespace arena_testgame
