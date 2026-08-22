#include "game_mcts/tournament_server/game_session.h"

#include <chrono>

namespace tournament_broker {

namespace {

auto NowUnixMs() -> int64_t {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

}  // namespace

void GameSession::RecordStep(int player, std::string action_bytes) {
  steps_.push_back(
      RecordedStep{.player = player,
                   .action_bytes = std::move(action_bytes),
                   .unix_ms = NowUnixMs()});
}

}  // namespace tournament_broker
