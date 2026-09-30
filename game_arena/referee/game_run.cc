#include "game_arena/referee/game_run.h"

#include <algorithm>
#include <cstddef>
#include <tuple>
#include <utility>

#include "absl/log/log.h"
#include "absl/strings/str_join.h"
#include "absl/time/clock.h"
#include "absl/time/time.h"

namespace tournament_broker {

GameRun::GameRun(const GameDescriptor& descriptor, GameRunConfig config,
                 std::vector<Seat> seats, uint64_t game_counter,
                 GameHistory* history, WorkerPool* pool, Timer* timer,
                 Task on_finished)
    : descriptor_(descriptor),
      config_(config),
      history_(history),
      timer_(timer),
      on_finished_(std::move(on_finished)),
      strand_(Strand::Create(pool)),
      game_id_("g" + std::to_string(absl::ToUnixMillis(absl::Now())) + "_" +
               std::to_string(game_counter)),
      seats_(std::move(seats)),
      session_(descriptor.new_session()),
      gen_(std::random_device{}() ^ static_cast<uint32_t>(game_counter)),
      time_used_(seats_.size()) {
  for (const Seat& seat : seats_) {
    clients_.push_back(seat.client);
  }
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
        self->Conclude(std::vector<int>(self->seats_.size()), std::move(reason),
                       /*tiebreak=*/false);
      });
}

void GameRun::Begin() {
  record_.set_game_id(game_id_);
  record_.set_game(descriptor_.name);
  for (const Seat& seat : seats_) {
    record_.add_player_names(seat.display_name);
  }
  record_.set_initial_state(session_->SerializeState());
  record_.set_initial_view(session_->RenderState());
  record_.set_started_unix_ms(absl::ToUnixMillis(absl::Now()));

  // Weak: seats_ owns the handles, so a strong reference would be a cycle.
  for (std::size_t seat = 0; seat < seats_.size(); ++seat) {
    if (!seats_[seat].client) {
      continue;
    }
    std::vector<std::string> others;
    for (std::size_t other = 0; other < seats_.size(); ++other) {
      if (other != seat) {
        others.push_back(seats_[other].display_name);
      }
    }
    seats_[seat].client->SetObserver([weak = weak_from_this()] {
      if (auto self = weak.lock()) {
        self->WakeStrand();
      }
    });
    proto::ServerMessage msg;
    auto* start = msg.mutable_game_start();
    start->set_game_id(game_id_);
    start->set_seat(static_cast<int>(seat));
    start->set_opponent_name(absl::StrJoin(others, ","));
    start->set_initial_state(record_.initial_state());
    *start->mutable_player_names() = record_.player_names();
    if (!seats_[seat].client->Send(msg)) {
      seats_[seat].client->MarkDisconnected();
    }
  }

  Step();
}

bool GameRun::SendYourTurn(int seat, std::chrono::milliseconds allowed) {
  proto::ServerMessage msg;
  auto* turn = msg.mutable_your_turn();
  turn->set_state(session_->SerializeState());
  turn->set_move_number(session_->MoveCount());
  turn->set_deadline_unix_ms(absl::ToUnixMillis(absl::Now()) + allowed.count());
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
      // Cancel() is best effort: the epoch catches a timer that fired anyway.
      if (self->concluded_ || epoch != self->turn_epoch_) {
        return;
      }
      const int seat = self->waiting_seat_;
      if (seat < 0) {
        return;
      }
      if (self->Forfeit(seat,
                        self->turn_budget_bound_ ? "time_budget" : "timeout")) {
        self->Step();
      }
    });
  });
}

void GameRun::CancelTurnTimer() {
  timer_->Cancel(turn_timer_);
  turn_timer_ = 0;
  ++turn_epoch_;  // invalidate anything already in flight
}

void GameRun::Step() {
  if (concluded_) {
    return;
  }

  for (;;) {
    CaptureViews();
    if (auto terminal = session_->Outcome()) {
      Conclude(std::move(terminal->places), "normal", /*tiebreak=*/true);
      return;
    }
    if (session_->MoveCount() >= config_.max_moves_per_game) {
      Conclude(session_->Standing().places, "max_moves", /*tiebreak=*/true);
      return;
    }
    if (session_->IsChanceNode()) {
      session_->ApplyChanceAction(gen_);
      continue;
    }

    const int seat = session_->CurrentPlayer();
    std::string action_bytes;

    if (seats_[seat].client != nullptr) {
      ClientHandle& client = *seats_[seat].client;
      if (client.disconnected()) {
        if (Forfeit(seat, "opponent_disconnect")) {
          continue;
        }
        return;
      }
      // Once per turn, before consuming: a pipelined action gets its YourTurn.
      if (waiting_seat_ != seat) {
        // Whichever of turn_timeout and the game budget runs out first.
        std::chrono::milliseconds allowed = config_.turn_timeout;
        turn_budget_bound_ = false;
        if (config_.game_time_budget.count() > 0) {
          const auto remaining = config_.game_time_budget - time_used_[seat];
          if (remaining <= std::chrono::milliseconds::zero()) {
            if (Forfeit(seat, "time_budget")) {
              continue;
            }
            return;
          }
          if (remaining < allowed) {
            allowed = std::chrono::ceil<std::chrono::milliseconds>(remaining);
            turn_budget_bound_ = true;
          }
        }
        if (!SendYourTurn(seat, allowed)) {
          client.MarkDisconnected();
          if (Forfeit(seat, "opponent_disconnect")) {
            continue;
          }
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
      time_used_[seat] += std::chrono::steady_clock::now() - turn_started_;
      waiting_seat_ = -1;
      CancelTurnTimer();
      action_bytes = std::move(*action);
    } else {
      const auto started = std::chrono::steady_clock::now();
      action_bytes = seats_[seat].builtin(session_->SerializeState(), gen_);
      time_used_[seat] += std::chrono::steady_clock::now() - started;
    }

    std::string error;
    if (!session_->ApplySerializedAction(action_bytes, &error)) {
      LOG(INFO) << "Game " << game_id_ << ": illegal action by seat " << seat
                << " (" << seats_[seat].display_name << "): " << error;
      // A builtin playing on for a forfeiter gets no builtin of its own.
      if (std::ranges::contains(forfeited_, seat)) {
        Conclude(session_->Standing().places, "illegal_action",
                 /*tiebreak=*/true);
        return;
      }
      if (Forfeit(seat, "illegal_action")) {
        continue;
      }
      return;
    }
  }
}

void GameRun::CaptureViews() {
  while (captured_.size() < session_->Steps().size()) {
    captured_.push_back(Captured{.caption = session_->RenderLastStep(),
                                 .view = session_->RenderState()});
  }
}

bool GameRun::Forfeit(int seat, std::string reason) {
  auto* forfeit = record_.add_forfeits();
  forfeit->set_seat(seat);
  forfeit->set_reason(reason);
  forfeit->set_move(session_->MoveCount());
  forfeited_.push_back(seat);
  if (forfeited_.size() + 1 == seats_.size()) {
    Conclude(std::vector<int>(seats_.size()), std::move(reason),
             /*tiebreak=*/false);
    return false;
  }
  LOG(INFO) << "Game " << game_id_ << ": seat " << seat << " ("
            << seats_[seat].display_name << ") forfeits (" << reason
            << "); builtin:" << descriptor_.forfeit_builtin << " plays on";
  CancelTurnTimer();
  waiting_seat_ = -1;
  std::string error;
  seats_[seat].client = nullptr;
  seats_[seat].builtin =
      descriptor_.make_builtin(descriptor_.forfeit_builtin, &error).value();
  return true;
}

void GameRun::Conclude(std::vector<int> places, std::string reason,
                       bool tiebreak) {
  if (concluded_) {
    return;
  }
  concluded_ = true;
  CancelTurnTimer();

  // Drop the observers so nothing reaches back into a finished game.
  for (const auto& client : clients_) {
    if (client) {
      client->SetObserver(nullptr);
    }
  }

  // No places from the game: all level.
  places.resize(seats_.size());
  const auto rank = [&](int seat) {
    const auto forfeit = std::ranges::find(forfeited_, seat);
    const auto late = forfeit == forfeited_.end()
                          ? 0
                          : static_cast<int>(forfeited_.end() - forfeit);
    return std::tuple(
        late, places[seat],
        tiebreak ? time_used_[seat] : std::chrono::steady_clock::duration{});
  };
  std::vector<int> order(seats_.size());
  for (std::size_t seat = 0; seat < order.size(); ++seat) {
    order[seat] = static_cast<int>(seat);
  }
  std::ranges::stable_sort(order, {}, rank);
  std::vector<int> placed(seats_.size());
  for (std::size_t i = 0; i < order.size(); ++i) {
    const bool level = i > 0 && rank(order[i]) == rank(order[i - 1]);
    placed[order[i]] = level ? placed[order[i - 1]] : static_cast<int>(i);
    // Level with the game's own places, but not after the clock: the time
    // tiebreak decided.
    if (i > 0 && !level && std::get<0>(rank(order[i])) == 0 &&
        std::get<1>(rank(order[i])) == std::get<1>(rank(order[i - 1]))) {
      reason = "time_tiebreak";
    }
  }

  CaptureViews();
  const std::vector<RecordedStep>& steps = session_->Steps();
  for (std::size_t i = 0; i < steps.size(); ++i) {
    auto* record_step = record_.add_steps();
    record_step->set_player(steps[i].player);
    record_step->set_action(steps[i].action_bytes);
    record_step->set_unix_ms(steps[i].unix_ms);
    record_step->set_view(std::move(captured_[i].view));
    record_step->set_caption(std::move(captured_[i].caption));
  }
  record_.set_termination_reason(reason);
  record_.set_finished_unix_ms(absl::ToUnixMillis(absl::Now()));
  const bool shared_first = std::ranges::count(placed, 0) > 1;
  record_.set_result(shared_first ? proto::GameRecord::DRAW
                                  : proto::GameRecord::WIN);
  record_.set_winning_player(
      shared_first
          ? -1
          : static_cast<int>(std::ranges::find(placed, 0) - placed.begin()));
  *record_.mutable_places() = {placed.begin(), placed.end()};

  history_->Store(record_);
  if (config_.on_record) {
    config_.on_record(record_);
  }

  LOG(INFO) << "Game " << game_id_ << " (" << descriptor_.name
            << ") finished: " << absl::StrJoin(record_.player_names(), " vs ")
            << " places=" << absl::StrJoin(placed, ",") << " reason=" << reason
            << " moves=" << session_->MoveCount();

  for (std::size_t seat = 0; seat < clients_.size(); ++seat) {
    if (!clients_[seat]) {
      continue;
    }
    ClientHandle& client = *clients_[seat];
    proto::ServerMessage msg;
    auto* over = msg.mutable_game_over();
    if (placed[seat] != 0) {
      over->set_result(proto::GameOver::LOSS);
    } else {
      over->set_result(shared_first ? proto::GameOver::DRAW
                                    : proto::GameOver::WIN);
    }
    // A forfeiter hears why it lost, not how the rest ended.
    const auto forfeit =
        std::ranges::find(record_.forfeits(), static_cast<int>(seat),
                          &proto::GameRecord::Forfeit::seat);
    over->set_reason(forfeit != record_.forfeits().end() ? forfeit->reason()
                                                         : reason);
    over->set_place(placed[seat]);
    client.Send(msg);
    client.CloseAfterFlush();
  }

  if (on_finished_) {
    Task done = std::move(on_finished_);
    std::move(done)();
  }
  // Last; the running strand task still holds a reference.
  self_.reset();
}

}  // namespace tournament_broker
