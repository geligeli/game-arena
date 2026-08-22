#ifndef RISK_GAME_AI_CPP_BENCHGAME_BENCH_SERIALIZATION_H
#define RISK_GAME_AI_CPP_BENCHGAME_BENCH_SERIALIZATION_H

#include "game_mcts/tournament_server/benchgame/bench.pb.h"
#include "game_mcts/tournament_server/benchgame/bench_game.h"
#include "game_mcts/cpp/mcts/serialization.h"

namespace mcts {

template <>
struct GameSerializationTraits<benchgame::BenchGame> {
  static constexpr bool kEnabled = true;
  using state_proto_t = benchgame::proto::BenchState;
  using action_proto_t = benchgame::proto::BenchAction;

  static auto StateToProto(const benchgame::BenchGame &state) -> state_proto_t {
    state_proto_t proto;
    proto.set_ply(state.ply);
    proto.set_current_player(state.current_player());
    return proto;
  }

  static auto StateFromProto(const state_proto_t &proto)
      -> benchgame::BenchGame {
    return benchgame::BenchGame{.ply = proto.ply()};
  }

  static auto ActionToProto(const benchgame::BenchGame::action_t &action)
      -> action_proto_t {
    action_proto_t proto;
    proto.set_value(action);
    return proto;
  }

  static auto ActionFromProto(const action_proto_t &proto)
      -> benchgame::BenchGame::action_t {
    return proto.value();
  }
};

static_assert(SerializableGame<benchgame::BenchGame>);

}  // namespace mcts

#endif  // RISK_GAME_AI_CPP_BENCHGAME_BENCH_SERIALIZATION_H
