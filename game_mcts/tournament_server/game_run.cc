#include "game_mcts/tournament_server/game_run.h"

#include <utility>

#include "absl/log/log.h"

namespace tournament_broker {

namespace {

auto NowUnixMs() -> int64_t {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

}  // namespace

GameRun::GameRun(const GameDescriptor &descriptor, GameRunConfig config,
                 std::array<Seat, 2> seats, uint64_t game_counter,
                 EloStore *elo_store, GameHistory *history, WorkerPool *pool,
                 Timer *timer, Task on_finished)
    : descriptor_(descriptor),
      config_(config),
      elo_store_(elo_store),
      history_(history),
      timer_(timer),
      on_finished_(std::move(on_finished)),
      strand_(Strand::Create(pool)),
      game_id_("g" + std::to_string(NowUnixMs()) + "_" +
               std::to_string(game_counter)),
      seats_(std::move(seats)),
      session_(descriptor.new_session()),
      gen_(std::random_device{}() ^ static_cast<uint32_t>(game_counter)) {}

auto GameRun::Create(const GameDescriptor &descriptor, GameRunConfig config,
                     std::array<Seat, 2> seats, uint64_t game_counter,
                     EloStore *elo_store, GameHistory *history,
                     WorkerPool *pool, Timer *timer, Task on_finished)
    -> std::shared_ptr<GameRun> {
  return std::shared_ptr<GameRun>(
      new GameRun(descriptor, config, std::move(seats), game_counter,
                  elo_store, history, pool, timer, std::move(on_finished)));
}

void GameRun::Start() {
  self_ = shared_from_this();
  strand_->Post([self = shared_from_this()] { self->Begin(); });
}

void GameRun::WakeStrand() {
  strand_->Post([self = shared_from_this()] { self->Step(); });
}

void GameRun::Abort(std::string reason) {
  strand_->Post(
      [self = shared_from_this(), reason = std::move(reason)]() mutable {
        if (self->concluded_) {
          return;
        }
        self->Conclude(GameOutcome{.is_draw = true}, std::move(reason));
      });
}

void GameRun::Begin() {
  record_.set_game_id(game_id_);
  record_.set_game(descriptor_.name);
  for (const Seat &seat : seats_) {
    record_.add_player_names(seat.display_name);
  }
  record_.set_initial_state(session_->SerializeState());
  record_.set_started_unix_ms(NowUnixMs());

  // Observers hold a weak reference: seats_ owns the handles, so a strong one
  // would be a cycle that never frees the game.
  for (int seat = 0; seat < 2; ++seat) {
    if (!seats_[seat].client) {
      continue;
    }
    seats_[seat].client->SetObserver([weak = weak_from_this()] {
      if (auto self = weak.lock()) {
        self->WakeStrand();
      }
    });
  }

  // Tell remote clients the game started.
  for (int seat = 0; seat < 2; ++seat) {
    if (!seats_[seat].client) {
      continue;
    }
    proto::ServerMessage msg;
    auto *start = msg.mutable_game_start();
    start->set_game_id(game_id_);
    start->set_seat(seat);
    start->set_opponent_name(seats_[1 - seat].display_name);
    start->set_initial_state(record_.initial_state());
    if (!seats_[seat].client->Send(msg)) {
      seats_[seat].client->MarkDisconnected();
    }
  }

  Step();
}

auto GameRun::SendYourTurn(int seat, std::chrono::milliseconds allowed)
    -> bool {
  proto::ServerMessage msg;
  auto *turn = msg.mutable_your_turn();
  turn->set_state(session_->SerializeState());
  turn->set_move_number(session_->MoveCount());
  turn->set_deadline_unix_ms(NowUnixMs() + allowed.count());
  return seats_[seat].client->Send(msg);
}

void GameRun::ArmTurnTimer(std::chrono::milliseconds delay) {
  const uint64_t epoch = ++turn_epoch_;
  turn_timer_ = timer_->After(delay, [weak = weak_from_this(), epoch] {
    auto self = weak.lock();
    if (!self) {
      return;
    }
    // Hop to the strand; the timer thread must never touch game state.
    self->strand_->Post([self, epoch] {
      // The epoch is the correctness backstop: Cancel() is best effort, so
      // a timer already being dispatched still fires and must be ignored.
      if (self->concluded_ || epoch != self->turn_epoch_) {
        return;
      }
      const int seat = self->waiting_seat_;
      if (seat < 0) {
        return;
      }
      self->Conclude(GameOutcome{.winning_player = 1 - seat},
                     self->turn_budget_bound_ ? "time_budget" : "timeout");
    });
  });
}

void GameRun::CancelTurnTimer() {
  if (turn_timer_ != 0) {
    timer_->Cancel(turn_timer_);
    turn_timer_ = 0;
  }
  ++turn_epoch_;  // invalidate anything already in flight
}

void GameRun::Step() {
  if (concluded_) {
    return;
  }

  for (;;) {
    if (const auto terminal = session_->Outcome()) {
      Conclude(*terminal, "normal");
      return;
    }
    if (session_->MoveCount() >= config_.max_moves_per_game) {
      Conclude(GameOutcome{.is_draw = true}, "max_moves");
      return;
    }
    if (session_->IsChanceNode()) {
      session_->ApplyChanceAction(gen_);
      continue;
    }

    const int seat = session_->CurrentPlayer();
    std::string action_bytes;

    if (seats_[seat].client != nullptr) {
      ClientHandle &client = *seats_[seat].client;
      if (client.disconnected()) {
        Conclude(GameOutcome{.winning_player = 1 - seat},
                 "opponent_disconnect");
        return;
      }
      // Ask once per turn, before consuming anything: a client that pipelined
      // an action still gets its YourTurn, exactly as the blocking loop did.
      if (waiting_seat_ != seat) {
        // The turn timeout bounds one move; the game budget bounds the whole
        // game. The deadline is whichever runs out first, and which one it was
        // decides the reason reported if it fires.
        std::chrono::milliseconds allowed = config_.turn_timeout;
        turn_budget_bound_ = false;
        if (config_.game_time_budget.count() > 0) {
          const auto remaining = config_.game_time_budget - time_used_[seat];
          if (remaining <= std::chrono::milliseconds::zero()) {
            Conclude(GameOutcome{.winning_player = 1 - seat}, "time_budget");
            return;
          }
          if (remaining < allowed) {
            allowed = remaining;
            turn_budget_bound_ = true;
          }
        }
        if (!SendYourTurn(seat, allowed)) {
          client.MarkDisconnected();
          Conclude(GameOutcome{.winning_player = 1 - seat},
                   "opponent_disconnect");
          return;
        }
        waiting_seat_ = seat;
        turn_started_ = std::chrono::steady_clock::now();
        ArmTurnTimer(allowed);
      }
      auto action = client.TryPopAction();
      if (!action.has_value()) {
        return;  // Resumes from the observer or the deadline.
      }
      time_used_[seat] += std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - turn_started_);
      waiting_seat_ = -1;
      CancelTurnTimer();
      action_bytes = std::move(*action);
    } else {
      // Runs on a pool worker. This is the CPU bound that replaced an
      // unbounded thread per game.
      action_bytes = seats_[seat].builtin(session_->SerializeState(), gen_);
    }

    std::string error;
    if (!session_->ApplySerializedAction(action_bytes, &error)) {
      LOG(INFO) << "Game " << game_id_ << ": illegal action by seat " << seat
                << " (" << seats_[seat].display_name << "): " << error;
      Conclude(GameOutcome{.winning_player = 1 - seat}, "illegal_action");
      return;
    }
  }
}

void GameRun::Conclude(GameOutcome outcome, std::string reason) {
  if (concluded_) {
    return;
  }
  concluded_ = true;
  CancelTurnTimer();

  // Drop the observers so nothing reaches back into a finished game.
  for (Seat &seat : seats_) {
    if (seat.client) {
      seat.client->SetObserver(nullptr);
    }
  }

  for (const RecordedStep &step : session_->Steps()) {
    auto *record_step = record_.add_steps();
    record_step->set_player(step.player);
    record_step->set_action(step.action_bytes);
    record_step->set_unix_ms(step.unix_ms);
  }
  record_.set_termination_reason(reason);
  record_.set_finished_unix_ms(NowUnixMs());
  const double score0 =
      outcome.is_draw ? 0.5 : (outcome.winning_player == 0 ? 1.0 : 0.0);
  record_.set_result(outcome.is_draw ? proto::GameRecord::DRAW
                                     : proto::GameRecord::WIN);
  record_.set_winning_player(outcome.winning_player);

  const auto new_elos =
      elo_store_->RecordResult(descriptor_.name, seats_[0].display_name,
                               seats_[1].display_name, score0);
  history_->Store(record_);

  LOG(INFO) << "Game " << game_id_ << " (" << descriptor_.name
            << ") finished: " << seats_[0].display_name << " vs "
            << seats_[1].display_name << " score0=" << score0
            << " reason=" << reason << " moves=" << session_->MoveCount();

  for (int seat = 0; seat < 2; ++seat) {
    if (!seats_[seat].client) {
      continue;
    }
    ClientHandle &client = *seats_[seat].client;
    proto::ServerMessage msg;
    auto *over = msg.mutable_game_over();
    if (outcome.is_draw) {
      over->set_result(proto::GameOver::DRAW);
    } else if (outcome.winning_player == seat) {
      over->set_result(proto::GameOver::WIN);
    } else {
      over->set_result(proto::GameOver::LOSS);
    }
    over->set_reason(reason);
    over->set_new_elo(seat == 0 ? new_elos.first : new_elos.second);
    client.Send(msg);
    client.CloseAfterFlush();
  }

  if (on_finished_) {
    Task done = std::move(on_finished_);
    std::move(done)();
  }
  // Last: the enclosing strand task still holds a reference, so this never
  // destroys the object mid-method.
  self_.reset();
}

}  // namespace tournament_broker
