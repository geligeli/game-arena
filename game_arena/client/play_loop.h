#ifndef GAME_ARENA_GAME_ARENA_CLIENT_PLAY_LOOP_H
#define GAME_ARENA_GAME_ARENA_CLIENT_PLAY_LOOP_H

// The client half of the Play stream, with the game left out.
//
// Joining a broker is the same work whatever the game: open the stream, send
// hello, answer every YourTurn with bytes, stop at GameOver, half-close so the
// server's handler exits. Only the choosing differs, and that is the callback.
//
// This is why a bot for a new problem is a function rather than a program. It
// is also the piece a problem's typed wrapper is built on: deserialize the
// state into your own type, decide, serialize the action.

#include <functional>
#include <random>
#include <string>
#include <string_view>

#include "game_arena/proto/tournament_broker.grpc.pb.h"

namespace tournament_client {

// Serialized state in, serialized action out. Deliberately the same shape as
// a registry's BuiltinFn: a built-in opponent and a remote client are the same
// kind of thing, and the broker cannot tell them apart.
using ChooseActionFn =
    std::function<std::string(std::string_view state_bytes, std::mt19937 &gen)>;

// Plays one game on its own stream. Returns false if the stream failed, not if
// the game was lost.
auto PlayOneGame(tournament_broker::proto::TournamentBroker::Stub *stub,
                 const std::string &name, const std::string &game,
                 const std::string &opponent, const ChooseActionFn &choose,
                 std::mt19937 &gen) -> bool;

// Plays |games| of them in sequence, stopping at the first stream failure.
auto PlayGames(tournament_broker::proto::TournamentBroker::Stub *stub,
               const std::string &name, const std::string &game,
               const std::string &opponent, int games,
               const ChooseActionFn &choose, std::mt19937 &gen) -> bool;

}  // namespace tournament_client

#endif  // GAME_ARENA_GAME_ARENA_CLIENT_PLAY_LOOP_H
