#pragma once

#include <qlib/common.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace qbook::testing {

struct WriterGoal {
  std::size_t orders = 0;
  std::size_t trades = 0;
  std::size_t cancels = 0;
  std::size_t levels = 0;
  std::optional<qlib::book> snapshot;
};

// Writer 独占计数；完成位仅证明目标记录已追加，文件仍须 Close 后读回。
class WriterGoalObserver {
public:
  explicit WriterGoalObserver(WriterGoal goal) : goal_(std::move(goal)) {}

  void Observe(const qlib::record& record) {
    std::visit([&](const auto& value) {
      using T = std::remove_cvref_t<decltype(value)>;
      if constexpr (std::is_same_v<T, qlib::book>) {
        if (!RawReached() || !goal_.snapshot || !time_seen_) return;
        const auto& expected = *goal_.snapshot;
        if (value.instrument_id != expected.instrument_id ||
            value.event_ns != event_ns_ || value.recv_ns != recv_ns_) return;
        for (std::size_t i = 0; i < qlib::snapshot_depth; ++i) {
          if (value.bid_price[i] != expected.bid_price[i] ||
              value.bid_qty[i] != expected.bid_qty[i] ||
              value.ask_price[i] != expected.ask_price[i] ||
              value.ask_qty[i] != expected.ask_qty[i]) return;
        }
        goal_reached_.store(true, std::memory_order_release);
      } else {
        constexpr auto index = [] {
          if constexpr (std::is_same_v<T, qlib::order>) return 0;
          else if constexpr (std::is_same_v<T, qlib::trade>) return 1;
          else if constexpr (std::is_same_v<T, qlib::cancel>) return 2;
          else return 3;
        }();
        const std::array expected{goal_.orders, goal_.trades, goal_.cancels, goal_.levels};
        if (seen_[index] >= expected[index]) throw std::runtime_error("Writer 原事件超过冻结目标");
        ++seen_[index];
        if (goal_.snapshot && value.instrument_id == goal_.snapshot->instrument_id) {
          event_ns_ = value.event_ns;
          recv_ns_ = value.recv_ns;
          time_seen_ = true;
        }
        if (RawReached() && !goal_.snapshot)
          goal_reached_.store(true, std::memory_order_release);
      }
    }, record);
  }

  bool Reached() const noexcept { return goal_reached_.load(std::memory_order_acquire); }

private:
  bool RawReached() const noexcept {
    return seen_ == std::array{goal_.orders, goal_.trades, goal_.cancels, goal_.levels};
  }
  WriterGoal goal_;
  std::array<std::size_t, 4> seen_{};
  std::int64_t event_ns_ = 0;
  std::int64_t recv_ns_ = 0;
  bool time_seen_ = false;
  std::atomic<bool> goal_reached_{false};
};

}  // namespace qbook::testing
