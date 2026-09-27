#include "game_arena/referee/game_session.h"

#include "absl/time/clock.h"
#include "absl/time/time.h"

namespace tournament_broker {

void GameSession::RecordStep(int player, std::string action_bytes) {
  steps_.push_back(RecordedStep{.player = player,
                                .action_bytes = std::move(action_bytes),
                                .unix_ms = absl::ToUnixMillis(absl::Now())});
}

}  // namespace tournament_broker
