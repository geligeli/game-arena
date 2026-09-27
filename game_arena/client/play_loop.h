#ifndef GAME_ARENA_GAME_ARENA_CLIENT_PLAY_LOOP_H
#define GAME_ARENA_GAME_ARENA_CLIENT_PLAY_LOOP_H

// The client half of the Play stream, with the choosing left to a callback.

#include <functional>
#include <random>
#include <string>
#include <string_view>

#include "game_arena/proto/tournament_broker.grpc.pb.h"

namespace tournament_client {

// The same shape as a registry's BuiltinFn on purpose.
using ChooseActionFn =
    std::function<std::string(std::string_view state_bytes, std::mt19937 &gen)>;

// One game on its own stream. False if the stream failed, not if it was lost.
bool PlayOneGame(
    tournament_broker::proto::TournamentBroker::StubInterface *stub,
    const std::string &name, const std::string &game,
    const std::string &opponent, const ChooseActionFn &choose,
    std::mt19937 &gen);

// Plays |games| of them in sequence, stopping at the first stream failure.
bool PlayGames(tournament_broker::proto::TournamentBroker::StubInterface *stub,
               const std::string &name, const std::string &game,
               const std::string &opponent, int games,
               const ChooseActionFn &choose, std::mt19937 &gen);

}  // namespace tournament_client

#endif  // GAME_ARENA_GAME_ARENA_CLIENT_PLAY_LOOP_H
